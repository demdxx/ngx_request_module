Nginx request module
====================

    @copyright 2012 Dmitry Ponomarev <demdxx@gmail.com>
    @license MIT http://opensource.org/licenses/MIT
    @libs libjansson https://github.com/akheron/jansson

Commands
--------

  **request_param** "VALUE";<br />
  # request_param   "Accept: application/json";<br />
  # request_param   "Content-Type: application/json";<br />
  # request_param   "Charsets: utf-8";<br />

  **request_agent** "User agent";

  **request_method** "GET POST PUT DELETE"; # As default

  **request** [GET POST PUT DELETE] URL [params or request body] $target_var;

  **request_echo** [GET POST PUT DELETE] URL [params or body]; # Put result to response

Example
-------

Get repositiry info;

```sh
location /repinfo {
    request GET
        "https://api.github.com/repos/demdxx/ngx_request_module"
        $module;
    json_extract $module
        $html_url $full_name $owner__url;
    ...
}
```

Install
-------

Like any other module Nginx.
Require libcurl.

```sh
./configure --add-module=<path-to-module>/ngx_request_module
make
```
