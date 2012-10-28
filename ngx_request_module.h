#ifndef NGX_REQUEST_MODULE
#define NGX_REQUEST_MODULE

#define MAX_BUFFER 2048 //2KB Buffers

typedef struct _data {
    char d[MAX_BUFFER];
    struct _data* next;
    int idx;
} ngx_curl_data;

typedef struct {
    ngx_str_t       agent;
    ngx_str_t       method;
    ngx_str_t       uri;
    ngx_str_t       body;
    ngx_array_t    *params;

    ngx_int_t       uri_index;
    ngx_int_t       body_index;

    bool            echo;
} ngx_request_subparams_t;

typedef struct {
    ngx_str_t       agent;
	ngx_str_t       method;
    ngx_array_t    *params;

    bool            handler_inited;

    ngx_array_t     sub_requests;
} ngx_request_module_loc_t;

extern ngx_module_t ngx_request_module;

#endif // NGX_REQUEST_MODULE