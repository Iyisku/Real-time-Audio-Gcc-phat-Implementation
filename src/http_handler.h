#ifndef HTTP_HANDLER_H
#define HTTP_HANDLER_H

#include <boost/asio.hpp>
#include <boost/beast.hpp>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

void handle_http_request(tcp::socket& socket, const http::request<http::string_body>& req);
std::string get_html();

#endif // HTTP_HANDLER_H