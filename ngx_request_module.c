#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <assert.h>
#include <curl/curl.h>

#include "ngx_request_module.h"

///////////////////////////////////////////////////////////////////////////////
/// DECLARE ///////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

// location configuration inits
static void *ngx_request_create_loc_conf(ngx_conf_t *cf);
static char *ngx_request_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);

// Config Commands
static char * ngx_request(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char * ngx_request_echo(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

// Variables
static ngx_int_t ngx_request_result(ngx_http_request_t *r, 
    ngx_http_variable_value_t *v, uintptr_t data);

// Handler
static ngx_int_t
ngx_request_echo_handler(ngx_http_request_t *r);

// Helpers
static ngx_request_subparams_t * ngx_request_begin(ngx_conf_t *cf, ngx_request_module_loc_t *olcf);
static size_t ngx_curl_callback(void *ptr, size_t size, size_t nmemb, void *userdata);
static void ngx_curl_data_clean(ngx_curl_data* d);

///////////////////////////////////////////////////////////////////////////////
/// INIT SECTION //////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

static ngx_command_t ngx_request_commands[] = {

    { ngx_string("request_agent"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_request_module_loc_t, agent),
      NULL },

    { ngx_string("request_method"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_request_module_loc_t, method),
      NULL },

    { ngx_string("request_param"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_array_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_request_module_loc_t, params),
      NULL },

    { ngx_string("request"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_2MORE,
      ngx_request,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("request_echo"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE123,
      ngx_request_echo,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};

static ngx_http_module_t ngx_request_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_request_create_loc_conf,           /* create location configuration */
    ngx_request_merge_loc_conf             /* merge location configuration */
};

ngx_module_t ngx_request_module = {
    NGX_MODULE_V1,
   &ngx_request_module_ctx,                /* module context */
    ngx_request_commands,                  /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

///////////////////////////////////////////////////////////////////////////////
/// IMPLEMENTATION ////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

// location configuration inits
static void *
ngx_request_create_loc_conf(ngx_conf_t *cf)
{
    ngx_request_module_loc_t  *olcf;

    olcf = ngx_pcalloc(cf->pool, sizeof(ngx_request_module_loc_t));
    if (NULL == olcf) {
        return NULL;
    }
    olcf->params = NGX_CONF_UNSET_PTR;

    return olcf;
}

static char *
ngx_request_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_request_module_loc_t *prev = parent;
    ngx_request_module_loc_t *conf = child;

    ngx_conf_merge_str_value(conf->agent,           prev->agent,            "");
    ngx_conf_merge_str_value(conf->method,          prev->method,           "");

    if (NULL==conf->params)
        conf->params = prev->params;

    return NGX_CONF_OK;
}

// Config Commands
static char *
ngx_request(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_request_module_loc_t        *olcf;
    ngx_request_subparams_t         *rq;
    ngx_str_t                       *value;
    ngx_http_variable_t             *v;
    ngx_int_t                       index;
    ngx_uint_t                      i;

    olcf = conf;
    rq = ngx_request_begin(cf, olcf);
    rq->echo = false;
    value = cf->args->elts;
    i = 1;

    // Method
    if (
        0 == ngx_strcmp(value[i].data, "GET")
     || 0 == ngx_strcmp(value[i].data, "POST")
     || 0 == ngx_strcmp(value[i].data, "PUT")
     || 0 == ngx_strcmp(value[i].data, "DELETE")
    ) {
        rq->method = value[i++];
    }

    // Request URI
    if ('$' == value[i].data[0]) {
        value[i].data++;
        value[i].len--;
        rq->uri_index = ngx_http_get_variable_index(cf, &value[i]);
        if (NGX_ERROR == rq->uri_index) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "Undefined var: [%V]", &value[i]);
            return NGX_CONF_ERROR;
        }
    } else {
        rq->uri = value[i];
    }
    i++;

    // Request body
    if (cf->args->nelts>i+1) {
        if ('$' == value[i].data[0]) {
            value[i].data++;
            value[i].len--;
            rq->body_index = ngx_http_get_variable_index(cf, &value[i]);
            if (NGX_ERROR == rq->body_index) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "Undefined var: [%V]", &value[i]);
                return NGX_CONF_ERROR;
            }
        } else {
            rq->body = value[i];
        }
        i++;
    }

    // Request target var
    if ('$' != value[i].data[0]) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "Invalid target var name: [%V]", &value[i]);
        return NGX_CONF_ERROR;
    }

    value[i].data++;
    value[i].len--;
    v = ngx_http_add_variable(cf, &value[i], NGX_HTTP_VAR_CHANGEABLE);
    if (NULL == v) {
        return NGX_CONF_ERROR;
    }
    
    index = ngx_http_get_variable_index(cf, &value[i]);
    if (NGX_ERROR == index) {
        return NGX_CONF_ERROR;
    }
    
    v->index        = index;
    v->data         = (uintptr_t) rq;
    v->get_handler  = ngx_request_result;

    return NGX_CONF_OK;
}


static char *
ngx_request_echo(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_request_module_loc_t        *olcf;
    ngx_http_core_loc_conf_t        *clcf;
    ngx_request_subparams_t         *rq;
    ngx_str_t                       *value;
    ngx_uint_t                      i;

    olcf = conf;

    if (!olcf->handler_inited) {
        olcf->handler_inited = true;
        clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
        clcf->handler = ngx_request_echo_handler;
    }

    rq = ngx_request_begin(cf, olcf);
    value = cf->args->elts;
    rq->echo = true;
    i = 1;

    // Method
    if (
        0 == ngx_strcmp(value[i].data, "GET")
     || 0 == ngx_strcmp(value[i].data, "POST")
     || 0 == ngx_strcmp(value[i].data, "PUT")
     || 0 == ngx_strcmp(value[i].data, "DELETE")
    ) {
        rq->method = value[i++];
    }

    // Request URI
    if ('$' == value[i].data[0]) {
        value[i].data++;
        value[i].len--;
        rq->uri_index = ngx_http_get_variable_index(cf, &value[i]);
        if (NGX_ERROR == rq->uri_index) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "Undefined var: [%V]", &value[i]);
            return NGX_CONF_ERROR;
        }
    } else {
        rq->uri = value[i];
    }
    i++;

    // Request body
    if (cf->args->nelts>i) {
        if ('$' == value[i].data[0]) {
            value[i].data++;
            value[i].len--;
            rq->body_index = ngx_http_get_variable_index(cf, &value[i]);
            if (NGX_ERROR == rq->body_index) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "Undefined var: [%V]", &value[i]);
                return NGX_CONF_ERROR;
            }
        } else {
            rq->body = value[i];
        }
        i++;
    }

    return NGX_CONF_OK;
}

// Vars
static ngx_int_t
ngx_request_result(ngx_http_request_t *r, 
    ngx_http_variable_value_t *v, uintptr_t data)
{
    CURL *handle;
    CURLcode resCode;
    ngx_int_t data_len;
    ngx_uint_t i;
    u_char *retVal;
    ngx_curl_data *storage;
    ngx_curl_data *curr_storage;
    ngx_http_variable_value_t *v2;
    ngx_request_subparams_t *rq = (ngx_request_subparams_t *)data;

    storage = malloc(sizeof(ngx_curl_data));
    storage->idx = 0;
    storage->next = 0;

    handle = curl_easy_init();
    if (NULL==handle) {
        ngx_log_error_core(NGX_LOG_EMERG, r->connection->log, 0,
            "Invalid init CURL handle");
        return NGX_ERROR;
    }

    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, ngx_curl_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, storage);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1);

    // Set URI
    if (rq->uri_index!=(ngx_int_t)NGX_CONF_UNSET_UINT) {
        v2 = ngx_http_get_indexed_variable(r, rq->uri_index);
        if (NULL==v2) {
            ngx_log_error_core(NGX_LOG_EMERG, r->connection->log, 0,
                "Invalid indexed var URI");
            return NGX_ERROR;
        }
        curl_easy_setopt(handle, CURLOPT_URL, v2->data);
    } else {
        curl_easy_setopt(handle, CURLOPT_URL, rq->uri.data);
    }

    // Set content body
    if (rq->body_index!=(ngx_int_t)NGX_CONF_UNSET_UINT) {
        v2 = ngx_http_get_indexed_variable(r, rq->body_index);
        if (NULL==v2) {
            ngx_log_error_core(NGX_LOG_EMERG, r->connection->log, 0,
                "Invalid indexed var BODY");
            return NGX_ERROR;
        }
        curl_easy_setopt(handle, CURLOPT_POST, 1);
        curl_easy_setopt(handle, CURLOPT_COPYPOSTFIELDS, v2->data);
    } else if (rq->body.len>0) {
        curl_easy_setopt(handle, CURLOPT_POST, 1);
        curl_easy_setopt(handle, CURLOPT_COPYPOSTFIELDS, rq->body.data);
    }

    // Set method
    if (rq->method.len>0) {
        if (0==ngx_strcmp("POST", rq->method.data)) {
            curl_easy_setopt(handle, CURLOPT_POST, 1);
        } else if (0==ngx_strcmp("GET", rq->method.data)) {
            curl_easy_setopt(handle, CURLOPT_HTTPGET, 1);
        } else {
            curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, rq->method.data);
        }
    }

    // Set agent
    if (rq->agent.len>0) {
        curl_easy_setopt(handle, CURLOPT_USERAGENT, rq->agent.data);
    }

    // Params
    if (NGX_CONF_UNSET_PTR!=rq->params) {
        struct curl_slist *headers = NULL;
        for (i=0 ; i<rq->params->nelts ; i++) {
            curl_slist_append(headers, (char*)((ngx_str_t*)rq->params->elts)[i].data);
        }
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    }

    // Process request
    resCode = curl_easy_perform(handle);
    if(resCode != 0) {
        ngx_log_error_core(NGX_LOG_EMERG, r->connection->log, 0,
            "Request error: %s", curl_easy_strerror(resCode));
        curl_easy_cleanup(handle);
        ngx_curl_data_clean(storage);
        return NGX_ERROR;
    }

    // Everything went OK.
    // How long is the data?
    data_len = 0;
    curr_storage = storage;
    while(curr_storage) {
        data_len += curr_storage->idx;
        curr_storage = curr_storage->next;
    }

    //Allocate storage
    retVal = ngx_palloc(r->pool, sizeof(char)*(data_len+1));
    retVal[data_len] = '\0';
    
    //Now copy in the data
    curr_storage = storage;
    data_len = 0;
    while(curr_storage) {
        memcpy(retVal+data_len, curr_storage->d, curr_storage->idx);
        data_len += curr_storage->idx;
        curr_storage = curr_storage->next;
    }

    //Cleanup
    curl_easy_cleanup(handle);
    ngx_curl_data_clean(storage);

    // Set var params
    v->len = data_len;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->data = retVal;

    return NGX_OK;
}

// Handler
static ngx_int_t ngx_request_echo_handler(ngx_http_request_t *r)
{
    ngx_uint_t                       i;
    ngx_request_subparams_t         *sparams;
    ngx_request_module_loc_t        *olcf;
    ngx_http_variable_value_t        v;
    ngx_buf_t                       *b;
    ngx_chain_t                     *out, *out2, *baseOut;

    olcf = ngx_http_get_module_loc_conf(r, ngx_request_module);
    if (NULL == olcf) {
        return NGX_DECLINED;
    }

    baseOut = NULL;
    out = NULL;

    if (NULL != olcf->sub_requests.elts) {
        sparams = olcf->sub_requests.elts;
        for (i=0 ; i<olcf->sub_requests.nelts ; i++) {
            if (!sparams[i].echo)
                continue;

            if (NGX_OK != ngx_request_result(r, &v, (uintptr_t)&sparams[i])) {
                return NGX_ERROR;
            }

            b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));

            b->pos = v.data;
            b->last = v.data + v.len;
            b->memory = 1;
            b->last_buf = 1;

            out2 = ngx_alloc_chain_link(r->pool);
            out2->buf = b;
            out2->next = NULL;
            if (NULL != out) {
                out->next = out2;
            } else {
                baseOut = out2;
            }
            out = out2;
        }
    }

    return NULL != baseOut
            ? ngx_http_output_filter(r, baseOut)
            : NGX_OK;
}

// Helpers
static ngx_request_subparams_t *
ngx_request_begin(ngx_conf_t *cf, ngx_request_module_loc_t *olcf)
{
    if (NULL==olcf->sub_requests.elts)
        if (ngx_array_init(&olcf->sub_requests, cf->pool, 1, sizeof(ngx_request_subparams_t))!=NGX_OK)
            return NULL;

    ngx_request_subparams_t *it = ngx_array_push(&olcf->sub_requests);

    it->agent = olcf->agent;
    it->method = olcf->method;
    it->params = olcf->params;
    it->uri_index = NGX_CONF_UNSET_UINT;
    it->body_index = NGX_CONF_UNSET_UINT;

    return it;
}

static size_t
ngx_curl_callback(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t idx;
    size_t max;
    ngx_curl_data* d;
    ngx_curl_data* nd;
    
    d = (ngx_curl_data*)userdata;

    idx = 0;
    max = nmemb * size;

    //Scan to the correct buffer
    while(NULL != d->next)
        d = d->next;

    //Store the data
    while(idx < max) {
        d->d[d->idx++] = ((char*)ptr)[idx++];

        if(d->idx == MAX_BUFFER) {
            nd = malloc(sizeof(ngx_curl_data));
            nd->next = NULL;
            nd->idx = 0;
            d->next = nd;
            d = nd;
        }
    }

    return max;
}

static void
ngx_curl_data_clean(ngx_curl_data* d)
{
    ngx_curl_data* pd;
    while (d) {
        pd = d->next;
        free(d);
        d = pd;
    }
}


