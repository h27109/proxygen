
#pragma once

#include <map>
#include <string>
#include <vector>

using std::map;
using std::string;
using std::vector;

typedef map<string, string> HttpHead;
typedef vector<HttpHead> HttpHeads;

class RequestHander {
 public:
    string GetOutput();
    string GetForwardUrl();
    bool GetForwardFlag();
    HttpHeads GetForwardHeaders();

    RequestHander &SetReqHeaders(HttpHeaders &headers) {
        req_headers_ = headers;
        return *this;
    }

    RequestHander &SetRequest(std::string &request) {
        request_ = request;
        return *this;
    }

 protected:
    void DoRequest();

 protected:
    HttpHeads req_heads_;
    std::string req_url_;
    std::string request_;
    
    int http_status_{200};

    HttpHeads forward_headers_;
    std::string out_msg_;
    bool forward_flag;
    std::string forward_url;
};

class ResponseHandler {
 protected:
    DoResponse();
};