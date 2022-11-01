#include "./event_loop.hpp"
#include "./hmac.hpp"
#include "./bucket.hpp"
#include "boost/url/url.hpp"
#include "boost/url/url_view.hpp"
#include "./connection.hpp"

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/detail/descriptor_ops.hpp>
#include <boost/asio/detail/socket_ops.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/detail/type_traits.hpp>
#include <boost/beast/http/dynamic_body.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/property_tree/detail/xml_parser_writer_settings.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include <boost/stacktrace.hpp>

#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/type_traits.hpp>

#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/stacktrace/stacktrace_fwd.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url/src.hpp>

#include <chrono>
#include <experimental/coroutine>
#include <filesystem>
#include <ios>
#include <iostream>
#include <fstream>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <irods/filesystem/collection_entry.hpp>
#include <irods/filesystem/collection_iterator.hpp>
#include <irods/filesystem/filesystem.hpp>
#include <irods/filesystem/filesystem_error.hpp>
#include <irods/filesystem/path.hpp>
#include <irods/genQuery.h>
#include <irods/msParam.h>
#include <irods/rcConnect.h>
#include <irods/rodsClient.h>
#include <irods/client_connection.hpp>
#include <irods/irods_client_api_table.hpp>
#include <irods/irods_pack_table.hpp>
#include <irods/irods_parse_command_line_options.hpp>
#include <irods/filesystem.hpp>
#include <irods/rcMisc.h>
#include <irods/rodsGenQuery.h>
#include <irods/rodsPath.h>
#include <irods/dstream.hpp>
#include <irods/transport/default_transport.hpp>
#include <irods/irods_query.hpp>
#include <irods/query_builder.hpp>

#include <memory>
#include <type_traits>

namespace asio = boost::asio;
namespace this_coro = boost::asio::this_coro;
namespace beast = boost::beast;
namespace fs = irods::experimental::filesystem;

using parser_type = boost::beast::http::parser<true, boost::beast::http::buffer_body>;

bool authenticate(rcComm_t* connection, const std::string_view& username);

std::string get_user_secret_key(rcComm_t* conn, const std::string_view& user)
{
    return "heck";
}

std::string
get_user_signing_key(const std::string_view& secret_key, const std::string_view& date, const std::string_view& region)
{
    // Hate it when amazon gives me homework
    // during implementation of their protocol
    std::cout << "date time component is " << date << std::endl;
    auto date_key = irods::s3::authentication::hmac_sha_256(std::string("AWS4").append(secret_key), date);
    auto date_region_key = irods::s3::authentication::hmac_sha_256(date_key, region);
    // 'date region service key'
    // :eyeroll:
    auto date_region_service_key = irods::s3::authentication::hmac_sha_256(date_region_key, "s3");
    return irods::s3::authentication::hmac_sha_256(date_region_service_key, "aws4_request");
}

bool authenticates(rcComm_t& conn, const parser_type& request, const boost::urls::url_view& url)
{
    std::vector<std::string> auth_fields;
    // should be equal to something like
    // [ 'AWS4-SHA256-HMAC Credential=...', 'SignedHeaders=...', 'Signature=...']
    //
    boost::split(auth_fields, request.get().at("Authorization"), boost::is_any_of(","));
}

asio::awaitable<void>
handle_listobjects_v2(asio::ip::tcp::socket& socket, parser_type& parser, const boost::urls::url_view& url)
{
    using namespace boost::property_tree;
    auto thing = irods::s3::get_connection();
    irods::experimental::filesystem::path resolved_path = irods::s3::resolve_bucket(url.segments()).c_str();
    boost::property_tree::ptree document;
    std::vector<std::string> args{std::string(resolved_path)};
    const auto query = fmt::format(
        "select COLL_NAME,DATA_NAME,DATA_OWNER_NAME,DATA_SIZE where COLL_NAME like '{}%'", resolved_path.c_str());
    auto contents = document.add("ListBucketResult", "");
    for (auto&& i : irods::query<RcComm>(thing.get(), query)) {
        ptree object;
        object.put("Key", irods::s3::strip_bucket(i[1]));
        object.put("Etag", i[1]);
        object.put("Owner", i[2]);
        object.put("Size", atoi(i[3].c_str()));
        // add_child always creates a new node, put_child would replace the previous one.
        document.add_child("ListBucketResult.Contents",object);
    }
    std::stringstream s;
    boost::property_tree::xml_parser::xml_writer_settings<std::string> settings;
    settings.indent_char = ' ';
    settings.indent_count = 4;
    boost::property_tree::write_xml(s, document, settings);
    boost::beast::http::response<boost::beast::http::string_body> response;
    response.body() = s.str();
    std::cout << s.str();
    std::cout << query << std::endl;
    boost::beast::http::write(socket, response);
    co_return;
}

asio::awaitable<void>
handle_getobject(asio::ip::tcp::socket& socket, parser_type& parser, const boost::urls::url_view& url)
{
    auto thing = irods::s3::get_connection();
    auto url_and_stuff = boost::urls::url_view(parser.get().base().target());
    // Permission verification stuff should go roughly here.

    fs::path path = irods::s3::resolve_bucket(url.segments()).c_str();
    std::cout << "Requested " << path << std::endl;

    try {
        if (fs::client::exists(*thing, path)) {
            std::cout << "Trying to write file" << std::endl;
            boost::beast::http::response<boost::beast::http::buffer_body> response;
            boost::beast::http::response_serializer<boost::beast::http::buffer_body> serializer{response};
            char buffer_backing[4096];
            response.result(boost::beast::http::status::accepted);
            std::string length_field =
                std::to_string(irods::experimental::filesystem::client::data_object_size(*thing, path));
            response.insert(boost::beast::http::field::content_length, length_field);
            auto md5 = irods::experimental::filesystem::client::data_object_checksum(*thing, path);
            response.insert("Content-MD5", md5);
            boost::beast::http::write_header(socket, serializer);
            boost::beast::error_code ec;
            irods::experimental::io::client::default_transport xtrans{*thing};
            irods::experimental::io::idstream d{xtrans, path};

            std::streampos current, size;
            while (d.good()) {
                d.read(buffer_backing, 4096);
                current = d.gcount();
                size += current;
                response.body().data = buffer_backing;
                response.body().size = current;
                std::cout << "Wrote " << current << " bytes" << std::endl;
                if (d.bad()) {
                    std::cerr << "Weird error?" << std::endl;
                    exit(12);
                }
                try {
                    boost::beast::http::write(socket, serializer);
                }
                catch (boost::system::system_error& e) {
                    if (e.code() != boost::beast::http::error::need_buffer) {
                        std::cout << "Not a good error!" << std::endl;
                        throw e;
                    }
                    else {
                        // It would be nice if we could figure out something a bit more
                        // semantic than catching an exception
                        std::cout << "Good error!" << std::endl;
                    }
                }
            }
            response.body().size = d.gcount();
            response.body().more = false;
            boost::beast::http::write(socket, serializer);
            std::cout << "Wrote " << size << " bytes total" << std::endl;
        }
        else {
            boost::beast::http::response<boost::beast::http::empty_body> response;
            response.result(boost::beast::http::status::not_found);
            std::cerr << "Could not find file" << std::endl;
            boost::beast::http::write(socket, response);
        }
    }
    catch (std::exception& e) {
        std::cout << boost::stacktrace::stacktrace() << std::endl;
        std::cout << "error! " << e.what() << std::endl;
    }

    boost::beast::http::response<boost::beast::http::dynamic_body> response;

    co_return;
}

// for now let's just list out what we get.
asio::awaitable<void> handle_request(asio::ip::tcp::socket socket)
{
    beast::http::parser<true, beast::http::buffer_body> parser;

    std::string buf_back;
    auto buffer = beast::flat_buffer();
    boost::system::error_code ec;
    std::cout << "Received " << beast::http::read_header(socket, buffer, parser, ec) << " bytes\n";
    if (ec) {
        // handle me please!
    }
    for (const auto& field : parser.get()) {
        std::cout << "header: " << field.name_string() << ":" << field.value() << std::endl;
    }
    std::cout << "target: " << parser.get().target() << std::endl;
    auto url = boost::urls::url_view(parser.get().base().target());
    const auto& segments = url.segments();
    const auto& params = url.params();
    std::cout << segments << " " << segments.empty() << std::endl;
    switch (parser.get().method()) {
        case boost::beast::http::verb::get:
            if (segments.empty() || params.contains("encoding-type") || params.contains("list-type")) {
                // Among other things, listobjects should be handled here.

                // This is a weird little thing because the parameters are a multimap.
                auto f = url.params().find("list-type");

                // Honestly not being able to use -> here strikes me as a potential mistake that
                // will be corrected in the future when boost::url is released properly as part
                // of boost
                if (f != url.params().end() && (*f).value == "2") {
                    co_await handle_listobjects_v2(socket, parser, url);
                }
            }
            else {
                // GetObject
                std::cout << "getobject detected" << std::endl;
                co_await handle_getobject(socket, parser, url);
            }
            break;
        case boost::beast::http::verb::put:
            if (parser.get().find("x-amz-copy-source") != parser.get().end()) {
                // copyobject
                std::cout << "Copyobject detected" << std::endl;
            }
            else {
                // putobject
                std::cout << "putobject detected" << std::endl;
            }
            break;
        case boost::beast::http::verb::head:
            // Probably just headbucket and headobject here.
            // and headbucket isn't on the immediate list
            // of starting point.
            if (url.segments().empty()) {
                std::cout << "Headbucket detected" << std::endl;
            }
            else {
                std::cout << "Headobject detected" << std::endl;
            }
            break;
        case boost::beast::http::verb::delete_:
            // DeleteObject
            std::cout << "Deleteobject detected" << std::endl;
            break;
        default:
            std::cerr << "Oh no..." << std::endl;
            exit(37);
            break;
    }
    char buf[512];
    while (!parser.is_done()) {
        std::cout << "Reading" << std::endl;
        parser.get().body().data = buf;
        parser.get().body().size = sizeof(buf);
        // Using async_read here causes it to completely eat the entire input without handling it properly.
        auto read = co_await beast::http::async_read_some(socket, buffer, parser, asio::use_awaitable);
        std::cout << "Read " << read << " bytes" << std::endl;
        if (ec == beast::http::error::need_buffer)
            ec = {};
        else if (ec)
            co_return;
    }
    // beast::http::response<beast::http::string_body> response;
    // response.body() = "Hi";
    // response.result(beast::http::status::ok);
    // response.insert("etag", "etag");
    // beast::http::response_serializer<beast::http::string_body> sr{response};
    // beast::http::write(socket, sr, ec);
    co_return;
}

asio::awaitable<void> listener()
{
    auto executor = co_await this_coro::executor;
    asio::ip::tcp::acceptor acceptor(executor, {asio::ip::tcp::v4(), 8080});
    for (;;) {
        asio::ip::tcp::socket socket = co_await acceptor.async_accept(boost::asio::use_awaitable);
        asio::co_spawn(executor, handle_request(std::move(socket)), asio::detached);
        std::cout << "Accepted?" << std::endl;
    }
}
int main()
{
    irods::api_entry_table& api_tbl = irods::get_client_api_table();
    irods::pack_entry_table& pk_tbl = irods::get_pack_table();
    init_api_table(api_tbl, pk_tbl);

    asio::io_context io_context(1);
    auto address = asio::ip::make_address("0.0.0.0");
    asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait([&](auto, auto) { io_context.stop(); });
    asio::co_spawn(io_context, listener(), boost::asio::detached);
    io_context.run();
    return 0;
}
