#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <search.h>
#include <sys/types.h>
#include <unistd.h>
#include <curl/curl.h>
#include <pthread.h>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/uri.h>
#include "lib/htab.h"

#define SEED_URL "http://ece252-1.uwaterloo.ca/lab4/"
#define ECE252_HEADER "X-Ece252-Fragment: "
#define BUF_SIZE 1048576  /* 1024*1024 = 1M */
#define BUF_INC  524288   /* 1024*512  = 0.5M */

#define CT_PNG  "image/png"
#define CT_HTML "text/html"
#define CT_PNG_LEN  9
#define CT_HTML_LEN 9

#define max(a, b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })



typedef struct recv_buf2 {
    char *buf;       /* memory to hold a copy of received data */
    size_t size;     /* size of valid data in buf in bytes*/
    size_t max_size; /* max capacity of buf in bytes*/
    int seq;         /* >=0 sequence number extracted from http header */
                     /* <0 indicates an invalid seq number */
} RECV_BUF; 

typedef struct p_inputs{
    int m;
    int i;
    int t;
}P_IN;


/* some heaps and arrays */
char url_frontier[1000][267]; //array of strings, each string is of size 267 characters and there are 1000 strings
char url_visited[1000][267]; 
char png_found[52][267]; 
struct hsearch_data * visited_url; //hash table of visited urls
char* all_keys[1000]; //copy of strings in visited_url, to be freed at the end of the program

/* sems, mutex, condition variables, and atomics */
pthread_mutex_t frontiers_mutex;
pthread_mutex_t png_mutex;
pthread_mutex_t visited_mutex;
pthread_mutex_t mutex_temp;
pthread_mutex_t mutex_num_png;
pthread_mutex_t mutex_first;
pthread_cond_t cond_var;

/* other global variable */
int frontier_size;
int num_url;
int num_png;
int frontier_pointer;
int threads_left;
int active;
int active_id[50];
int exit_now;

//helper functions for curl calls
htmlDocPtr mem_getdoc(char *buf, int size, const char *url);
xmlXPathObjectPtr getnodeset (xmlDocPtr doc, xmlChar *xpath);
int find_http(char *fname, int size, int follow_relative_links, const char *base_url);
size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata);
size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata);
int recv_buf_init(RECV_BUF *ptr, size_t max_size);
int recv_buf_cleanup(RECV_BUF *ptr);
void cleanup(CURL *curl, RECV_BUF *ptr);
int write_file(const char *path, const void *in, size_t len);
CURL *easy_handle_init(RECV_BUF *ptr, const char *url);
int process_data(CURL *curl_handle, RECV_BUF *p_recv_buf, int m_png);


htmlDocPtr mem_getdoc(char *buf, int size, const char *url)
{
    int opts = HTML_PARSE_NOBLANKS | HTML_PARSE_NOERROR | \
               HTML_PARSE_NOWARNING | HTML_PARSE_NONET;
    htmlDocPtr doc = htmlReadMemory(buf, size, url, NULL, opts);
    
    if ( doc == NULL ) {
        // fprintf(stderr, "Document not parsed successfully.\n");
        return NULL;
    }
    return doc;
}

xmlXPathObjectPtr getnodeset (xmlDocPtr doc, xmlChar *xpath)
{
	
    xmlXPathContextPtr context;
    xmlXPathObjectPtr result;

    context = xmlXPathNewContext(doc);
    if (context == NULL) {
        printf("Error in xmlXPathNewContext\n");
        return NULL;
    }
    result = xmlXPathEvalExpression(xpath, context);
    xmlXPathFreeContext(context);
    if (result == NULL) {
        printf("Error in xmlXPathEvalExpression\n");
        return NULL;
    }
    if(xmlXPathNodeSetIsEmpty(result->nodesetval)){
        xmlXPathFreeObject(result);
        // printf("No result\n");
        return NULL;
    }
    return result;
}

int is_png(const void *buf){
    unsigned char png_id[8] = {0x89,0x50,0x4E,0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    unsigned char cmp_val[8];

    memcpy(cmp_val, buf, 8);

    if(!memcmp(png_id,cmp_val, 8)){
        return 1;
    }

    return 0;
}


int find_http(char *buf, int size, int follow_relative_links, const char *base_url) 
{

    int i;
    htmlDocPtr doc;
    xmlChar *xpath = (xmlChar*) "//a/@href";
    xmlNodeSetPtr nodeset;
    xmlXPathObjectPtr result;
    xmlChar *href;
    

		
    if (buf == NULL) {
        return 1;
    }

    doc = mem_getdoc(buf, size, base_url);
    result = getnodeset (doc, xpath);
    if (result) {

        nodeset = result->nodesetval;
        
        for (i=0; i < nodeset->nodeNr; i++) {
            href = xmlNodeListGetString(doc, nodeset->nodeTab[i]->xmlChildrenNode, 1);
            if ( follow_relative_links ) {
                xmlChar *old = href;
                href = xmlBuildURI(href, (xmlChar *) base_url);
                xmlFree(old);

            }
            if ( href != NULL && !strncmp((const char*) href, "http", 4) ) { //checking if first 4 characters in link is http
                // printf("href: %s\n", href);
                char check_url[267]; //copies to char array to free at the end
                strcpy(check_url, (char*) href);
        
                /* add discovered urls to the frontier */
               
                if(!hash_table_get(check_url, visited_url)){ //check if url is already in visited_url
                    pthread_mutex_lock(&visited_mutex);
                    strcpy(url_frontier[frontier_size],check_url); //if url is not in hash table, add to url_frontier
                    ++frontier_size;
                    pthread_mutex_unlock(&visited_mutex);
                }
                

            }
            xmlFree(href);
        }
        xmlXPathFreeObject (result);
    }
    pthread_mutex_lock(&mutex_temp);
    xmlFreeDoc(doc);
    xmlCleanupParser();
    pthread_mutex_unlock(&mutex_temp);
    return 0;
}
/**
 * @brief  cURL header call back function to extract image sequence number from 
 *         http header data. An example header for image part n (assume n = 2) is:
 *         X-Ece252-Fragment: 2
 * @param  char *p_recv: header data delivered by cURL
 * @param  size_t size size of each memb
 * @param  size_t nmemb number of memb
 * @param  void *userdata user defined data structurea
 * @return size of header data received.
 * @details this routine will be invoked multiple times by the libcurl until the full
 * header data are received.  we are only interested in the ECE252_HEADER line 
 * received so that we can extract the image sequence number from it. This
 * explains the if block in the code.
 */
size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata)
{
    int realsize = size * nmemb;
    RECV_BUF *p = userdata;

#ifdef DEBUG1_
    printf("%s", p_recv);
#endif /* DEBUG1_ */
    if (realsize > strlen(ECE252_HEADER) &&
	strncmp(p_recv, ECE252_HEADER, strlen(ECE252_HEADER)) == 0) {

        /* extract img sequence number */
	p->seq = atoi(p_recv + strlen(ECE252_HEADER));

    }
    return realsize;
}


/**
 * @brief write callback function to save a copy of received data in RAM.
 *        The received libcurl data are pointed by p_recv, 
 *        which is provided by libcurl and is not user allocated memory.
 *        The user allocated memory is at p_userdata. One needs to
 *        cast it to the proper struct to make good use of it.
 *        This function maybe invoked more than once by one invokation of
 *        curl_easy_perform().
 */

size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata)
{
    size_t realsize = size * nmemb;
    RECV_BUF *p = (RECV_BUF *)p_userdata;
 
    if (p->size + realsize + 1 > p->max_size) {/* hope this rarely happens */ 
        /* received data is not 0 terminated, add one byte for terminating 0 */
        size_t new_size = p->max_size + max(BUF_INC, realsize + 1);   
        char *q = realloc(p->buf, new_size);
        if (q == NULL) {
            perror("realloc"); /* out of memory */
            return -1;
        }
        p->buf = q;
        p->max_size = new_size;
    }

    memcpy(p->buf + p->size, p_recv, realsize); /*copy data from libcurl*/
    p->size += realsize;
    p->buf[p->size] = 0;

    return realsize;
}


int recv_buf_init(RECV_BUF *ptr, size_t max_size)
{
    void *p = NULL;
    
    if (ptr == NULL) {
        return 1;
    }

    p = malloc(max_size);
    memset(p, 0, sizeof(p));
    if (p == NULL) {
	return 2;
    }
    
    ptr->buf = p;
    ptr->size = 0;
    ptr->max_size = max_size;
    ptr->seq = -1;              /* valid seq should be positive */
    return 0;
}

int recv_buf_cleanup(RECV_BUF *ptr)
{
    if (ptr == NULL) {
	return 1;
    }
    
    free(ptr->buf);
    ptr->buf =NULL;
    ptr->size = 0;
    ptr->max_size = 0;
    return 0;
}

void cleanup(CURL *curl, RECV_BUF *ptr)
{
        curl_easy_cleanup(curl);
        curl = NULL;
        // curl_global_cleanup();
        recv_buf_cleanup(ptr);
        ptr = NULL;
}
/**
 * @brief output data in memory to a file
 * @param path const char *, output file path
 * @param in  void *, input data to be written to the file
 * @param len size_t, length of the input data in bytes
 */

int write_file(const char *path, const void *in, size_t len)
{
    FILE *fp = NULL;

    if (path == NULL) {
        fprintf(stderr, "write_file: file name is null!\n");
        return -1;
    }

    if (in == NULL) {
        fprintf(stderr, "write_file: input data is null!\n");
        return -1;
    }

    fp = fopen(path, "wb");
    if (fp == NULL) {
        perror("fopen");
        return -2;
    }

    if (fwrite(in, 1, len, fp) != len) {
        fprintf(stderr, "write_file: imcomplete write!\n");
        return -3; 
    }
    return fclose(fp);
}

/**
 * @brief create a curl easy handle and set the options.
 * @param RECV_BUF *ptr points to user data needed by the curl write call back function
 * @param const char *url is the target url to fetch resoruce
 * @return a valid CURL * handle upon sucess; NULL otherwise
 * Note: the caller is responsbile for cleaning the returned curl handle
 */

/*
static void init(CURLM *cm, int i)
{
  CURL *eh = curl_easy_init();
  curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, cb);
  curl_easy_setopt(eh, CURLOPT_HEADER, 0L);
  curl_easy_setopt(eh, CURLOPT_URL, urls[i]);
  curl_easy_setopt(eh, CURLOPT_PRIVATE, urls[i]);
  curl_easy_setopt(eh, CURLOPT_VERBOSE, 0L);
  curl_multi_add_handle(cm, eh);
}
*/

CURL *easy_handle_init(RECV_BUF *ptr, const char *url)
{
    CURL *curl_handle = NULL;

    if ( ptr == NULL || url == NULL) {
        return NULL;
    }

    /* init user defined call back function buffer */
    if ( recv_buf_init(ptr, BUF_SIZE) != 0 ) {
        return NULL;
    }
    /* init a curl session */
    curl_handle = curl_easy_init();

    if (curl_handle == NULL) {
        fprintf(stderr, "curl_easy_init: returned NULL\n");
        return NULL;
    }

    /* specify URL to get */
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);

    /* register write call back function to process received data */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb_curl3); 
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)ptr);

    /* register header call back function to process received header data */
    curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_cb_curl); 
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *)ptr);

    /* some servers requires a user-agent field */
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "ece252 lab4 crawler");

    /* follow HTTP 3XX redirects */
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    /* continue to send authentication credentials when following locations */
    curl_easy_setopt(curl_handle, CURLOPT_UNRESTRICTED_AUTH, 1L);
    /* max numbre of redirects to follow sets to 5 */
    curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 5L);
    /* supports all built-in encodings */ 
    curl_easy_setopt(curl_handle, CURLOPT_ACCEPT_ENCODING, "");

    /* Max time in seconds that the connection phase to the server to take */
    //curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 5L);
    /* Max time in seconds that libcurl transfer operation is allowed to take */
    //curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L);
    /* Time out for Expect: 100-continue response in milliseconds */
    //curl_easy_setopt(curl_handle, CURLOPT_EXPECT_100_TIMEOUT_MS, 0L);

    /* Enable the cookie engine without reading any initial cookies */
    curl_easy_setopt(curl_handle, CURLOPT_COOKIEFILE, "");
    /* allow whatever auth the proxy speaks */
    curl_easy_setopt(curl_handle, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
    /* allow whatever auth the server speaks */
    curl_easy_setopt(curl_handle, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
    
    return curl_handle;
}

int process_html(CURL *curl_handle, RECV_BUF *p_recv_buf)
{
    int follow_relative_link = 1;
    char *url = NULL; 

    curl_easy_getinfo(curl_handle, CURLINFO_EFFECTIVE_URL, &url);
    find_http(p_recv_buf->buf, p_recv_buf->size, follow_relative_link, url); 
    return 0;
}

int process_png(CURL *curl_handle, RECV_BUF *p_recv_buf, int m_png)
{
    char *eurl = NULL;          /* effective URL */
    curl_easy_getinfo(curl_handle, CURLINFO_EFFECTIVE_URL, &eurl);

    if ( eurl != NULL) {
        // printf("The PNG url is: %s\n", eurl);
    }

    /* add png to found array and hash table */
        
        pthread_mutex_lock(&png_mutex);
        if( is_png(p_recv_buf->buf) && num_png < m_png ){
     
            strcpy(png_found[num_png], eurl);
            printf("The PNG url is %d: %s\n",num_png, eurl);
            num_png++;
        }
        pthread_mutex_unlock(&png_mutex);
        
        

    return 0;
}
/**
 * @brief process the download data by curl
 * @param CURL *curl_handle is the curl handler
 * @param RECV_BUF p_recv_buf contains the received data. 
 * @return 0 on success; non-zero otherwise
 */

int process_data(CURL *curl_handle, RECV_BUF *p_recv_buf, int m_png)
{
    CURLcode res;
    // char fname[256];
    long response_code;

    res = curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);
    if ( res == CURLE_OK ) {
	    printf("Response code: %ld\n", response_code);
    }

    if ( response_code >= 400 ) { 
    	// fprintf(stderr, "Error.\n");
        return 1;
    }

    char *ct = NULL;
    res = curl_easy_getinfo(curl_handle, CURLINFO_CONTENT_TYPE, &ct);
    if ( res == CURLE_OK && ct != NULL ) {
    	printf("Content-Type: %s, len=%ld\n", ct, strlen(ct));
    } else {
        fprintf(stderr, "Failed obtain Content-Type\n");
        return 2;
    }
   
    if ( strstr(ct, CT_HTML) ) {
        return process_html(curl_handle, p_recv_buf);
    } else if ( strstr(ct, CT_PNG) ) {
        return process_png(curl_handle, p_recv_buf, m_png);
    } 

    // return write_file(fname, p_recv_buf->buf, p_recv_buf->size);
    return 0;
}


void* curl(void* arg){
    CURL *curl_handle = NULL;
    CURLcode res;
    RECV_BUF recv_buf;
    recv_buf.buf =NULL;
    char temp_url[256];

    P_IN* temp_in = arg; //inputs 

    while( 1 ){

    // printf("num_png: %d\n\n", num_png);
    // printf("num_url: %d\n\n", num_url);
    //printf("frontier_pointer: %d\n\n", frontier_pointer);
    //printf("frontier size: %d\n", frontier_size);


    pthread_mutex_lock(&frontiers_mutex);


      
    if( active > (frontier_size - frontier_pointer) && (frontier_size - frontier_pointer) >= 0 ){ /* wait for space in the frontier */
        // __sync_add_and_fetch(&threads_left, 1);

        if(active_id[temp_in->i] == 1){
            active_id[temp_in->i] = 0;
            __sync_sub_and_fetch(&active, 1);
            // printf("%d num active thread DECREASED: %d\n",temp_in->i, active);
        }

        // printf(" SLEEPING %d\n", temp_in->i);
        pthread_cond_wait(&cond_var, &frontiers_mutex); 
        // __sync_sub_and_fetch(&threads_left, 1);
    }
    
        pthread_mutex_lock(&mutex_first);
        if(num_png >= temp_in->m || (frontier_size - frontier_pointer) < 0 ||  strlen(url_frontier[frontier_pointer]) == 0){
            // printf("exiting . . . \n");
            --active;
            --threads_left;
            // __sync_sub_and_fetch(&threads_left, 1);
            pthread_mutex_unlock(&frontiers_mutex);
            pthread_mutex_unlock(&mutex_first);
            pthread_cond_broadcast(&cond_var);
            break;
        }
        
        // printf(" HELLO %d\n", temp_in->i);

        if(active_id[temp_in->i] == 0){
            ++active;
            active_id[temp_in->i] = 1;
        }

        if(active < (frontier_size - frontier_pointer)){

        pthread_cond_signal(&cond_var);
        }else if(active == (frontier_size - frontier_pointer)){
            
        } else{
            if( !exit_now ){
                // printf("too many threads running !\n");
                // printf("num active thread 1 sent back: %d\n", active);
                pthread_mutex_unlock(&mutex_first);
                pthread_mutex_unlock(&frontiers_mutex);
                continue;
            }
        
        }
        pthread_mutex_unlock(&mutex_first);

        pthread_mutex_unlock(&frontiers_mutex);
        
        pthread_mutex_lock(&mutex_num_png);
        strcpy(temp_url, url_frontier[frontier_pointer]);


        if(!hash_table_get(temp_url, visited_url)){
             /* add current url to the visted hash table + array */
            hash_table_add(temp_url, 2, visited_url, &all_keys[num_url]);
            strcpy(url_visited[num_url], temp_url);
            ++frontier_pointer;
            ++num_url;
        }else{
              ++frontier_pointer;
              pthread_mutex_unlock(&mutex_num_png);
            continue;
        }
        
        
        cleanup(curl_handle, &recv_buf);
        curl_handle = easy_handle_init(&recv_buf, temp_url);

        pthread_mutex_unlock(&mutex_num_png);

        // printf("%d URL is %s\n", temp_in->i, temp_url);
        // if ( curl_handle == NULL ) {
        //     fprintf(stderr, "Curl initialization failed. Exiting...\n");
        //     curl_global_cleanup();
        //     abort();
        // }
        /* get it! */
        res = curl_easy_perform(curl_handle);

        if( res != CURLE_OK) {
            if( res == CURLE_COULDNT_CONNECT){ 
            // free(all_keys[num_url -1]);
            // __sync_sub_and_fetch(&num_url, 1);
            strcpy(url_visited[num_url -1], " ");
            continue; 
            }
            else{
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            exit(1);
            }
        } else {
        // printf("%lu bytes received in memory %p, seq=%d.\n", 
        //         recv_buf.size, recv_buf.buf, recv_buf.seq);
        }



        /* process the download data */
        process_data(curl_handle, &recv_buf, temp_in->m);

        if(frontier_size <= frontier_pointer || num_png == temp_in->m){
             __sync_sub_and_fetch(&threads_left, 1);
             exit_now = 1;
            // printf(" num threads left: %d\n", threads_left);
            // while(threads_left > 0){
            //     // pthread_mutex_unlock(&visited_mutex);
            //     // pthread_mutex_unlock(&png_mutex);
            //     // printf(" num threads left: %d\n", threads_left);
            //     pthread_cond_broadcast(&cond_var);
                
            // }
            // printf(" tread exiting: %d\n", temp_in->i);
            pthread_cond_broadcast(&cond_var);
            pthread_mutex_unlock(&frontiers_mutex);
        break;
        }

    }
    // while(threads_left > 0){}

    /* cleaning up */
    cleanup(curl_handle, &recv_buf);
    pthread_exit(0);
}




int main( int argc, char** argv ) {   
    int c;
    int t = 1;
    int m = 50;
    char *v = NULL;
    char *str = "option requires an argument";

    while ((c = getopt (argc, argv, "t:m:v:")) != -1) {
        switch (c) {
        case 't':
	    t = strtoul(optarg, NULL, 10);
	    // printf("option -t specifies a value of %d.\n", t);
	    if (t <= 0) {
                fprintf(stderr, "%s: %s > 0 -- 't'\n", argv[0], str);
                return -1;
            }
            break;
        case 'm':
            m = strtoul(optarg, NULL, 10);
	        // printf("option -m specifies a value of %d.\n", m);
            if (m <= 0 ) {
                fprintf(stderr, "%s: %s > 0 -- 'm'\n", argv[0], str);
                return -1;
            }
            break;
        case 'v':
            v = optarg;
            // printf("option -v specifies a filename of %s\n", v);
            
            
            break;
      
        default:
            return -1;
        }
    }

    double times[2];
    struct timeval tv;
    char url[256];
    pthread_t tid[t];
    P_IN p_in[t];

    if (optind < argc){
		for (; optind < argc; optind++){
			strcpy(url, argv[optind]);
        }
	}else {
		strcpy(url, SEED_URL); 
	}
        
    if (argc == 1) {
        strcpy(url, SEED_URL); 
    } 

    /* push seed into frontier heap*/
    strcpy(url_frontier[0], url);

    /* initilaze global variables */
    frontier_size = 1;
    num_url = 0;
    num_png = 0;
    frontier_pointer = 0;
    threads_left = t;
    active = 0;
    exit_now = 0;

    for(int i = 0; i < 50; ++i){
        active_id[i]=0;
    }

    // for(int i = 0; i < 1000; ++i){
    //     all_keys[i]=malloc(sizeof(char*));
    // }

    
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* initialize hash table */
    visited_url = calloc(1, hash_table_size());
    // found_png = calloc(1, hash_table_size());
    hash_table_init(visited_url);

   
    /* initializing sems, mutex, conditions variables and atomics */
    pthread_mutex_init(&frontiers_mutex, NULL);
    pthread_cond_init(&cond_var, NULL);
    pthread_mutex_init(&visited_mutex, NULL);
    pthread_mutex_init(&mutex_temp, NULL);
    pthread_mutex_init(&mutex_num_png, NULL);
    pthread_mutex_init(&mutex_first, NULL);

     /* start timer */
    if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
    }
    times[0] = (tv.tv_sec) + tv.tv_usec/1000000.;

    for ( int i = 0; i< t; ++i){
        p_in[i].i = i;
        p_in[i].m = m;
        p_in[i].t = t;
        pthread_create(&tid[i], NULL, curl, p_in + i);
    }

    for(int i = 0; i<t; ++i){
        pthread_join(tid[i], NULL);
    }

    /* new line marker */
    char new_line[2];
    sprintf(new_line, "\n");

    /* make png_urls.txt file*/
    FILE* p_url = NULL;
    p_url = fopen ("png_urls.txt", "wb");

    for(int i = 0; i < num_png ; ++i){
        fwrite(png_found[i], 1, strlen(png_found[i]), p_url);
        fwrite(&new_line, 1, 1, p_url);
    }

    fclose(p_url);

    if( v != NULL ){
        /* make log.txt file*/
        FILE* log_text = NULL;
        log_text = fopen (v, "wb");
        for(int i = 0; i < num_url ; ++i){

        if(strcmp(url_visited[i], " ")){
             fwrite(url_visited[i], 1, strlen(url_visited[i]), log_text);
            fwrite(&new_line, 1, 1, log_text);
        }

        }
        fclose(log_text);
    }

    /* end timer */
    if (gettimeofday(&tv, NULL) != 0) {
            perror("gettimeofday");
            abort();
        }
    times[1] = (tv.tv_sec) + tv.tv_usec/1000000.;
    printf("findpng2 execution time: %.6lf seconds\n", times[1] - times[0]);

    curl_global_cleanup();
    // pthread_mutex_destroy(&frontiers_mutex);
    // pthread_mutex_destroy(&visited_mutex);
    // pthread_mutex_destroy(&mutex_temp);

    for(int i = 0; i < 1000; ++i){
        free(all_keys[i]);
    }

    hdestroy_r(visited_url);
    free(visited_url);

    return 0;
}
