#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <unistd.h>
#include <iostream>
#include <filesystem>
#include <poll.h>
#include <string>
#include <vector>
#include <algorithm>
#include <regex>
#include <fstream>
#include <fcntl.h>

#include "err.h"

#define BUFFER_SIZE     2048
#define QUEUE_LENGTH    20
#define MAX_SIZE        33554432
#define TIMEOUT         120000          // milliseconds for timeout

#define CRLF            "\r\n"
#define PATH            "([a-zA-Z0-9\\.\\/-])*"
#define REQ_LINE        "(GET|HEAD)( )([^ ])*( )(HTTP\\/1\\.1)"

#define PROT            "http://"
#define HTTP_VERSION    "HTTP/1.1"


using namespace std;

string folder;
string corelated;
int msg_sock;
bool conn_close, res_send;


void writeToMsgSocket(string const &msg) {
    res_send = true;
    if (send(msg_sock, msg.c_str(), msg.size(), MSG_NOSIGNAL) < (int)msg.size()) {
        conn_close = true;
        return;
    }
    cout << "Status-line send:" << endl;
    cout << msg << endl;
}

void sendStatus200(size_t file_size) {
    stringstream msg;
    msg << HTTP_VERSION << " " << "200" << " " << "OK" << CRLF;
    if (conn_close) {
        msg << "Connection: close" << CRLF;
    }
    msg << "Content-type: application/octet-stream" << CRLF;
    msg << "Content-length: " << file_size << CRLF;
    msg << CRLF;
    writeToMsgSocket(msg.str());
};

void sendStatus302(string const &serwer, string const &port, string const &target) {
    stringstream msg;
    msg << HTTP_VERSION << " " << "302" << " " << "OK" << CRLF;
    if (conn_close) {
        msg << "Connection: close" << CRLF;
    }
    msg << "Location: " << PROT << serwer << ":" << port << target << CRLF;
    msg << CRLF;
    writeToMsgSocket(msg.str());
};

void sendStatus400(string const &reason) {
    conn_close = true;
    stringstream  msg;
    msg << HTTP_VERSION << " " << "400" << " " << reason << CRLF;
    msg << "Connection: close" << CRLF;
    msg << CRLF;
    writeToMsgSocket(msg.str());
};

void sendStatus404() {
    stringstream msg;
    msg << HTTP_VERSION << " " << "404" << " " << "File not found" << CRLF;
    if (conn_close) {
        msg << "Connection: close" << CRLF;
    }
    msg << CRLF;
    writeToMsgSocket(msg.str());
};

void sendStatus500() {
    conn_close = true;
    stringstream msg;
    msg << HTTP_VERSION << " " << "500" << " " << "Internal server error" << CRLF;
    msg << "Connection: close" << CRLF;
    msg << CRLF;
    writeToMsgSocket(msg.str());
}

void sendStatus501() {
    conn_close = true;
    stringstream msg;
    msg << HTTP_VERSION << " " << "501" << " " << "Method not recognized" << CRLF;
    msg << "Connection: close" << CRLF;
    msg << CRLF;
    writeToMsgSocket(msg.str());
}


// Check for path correctness.
bool is_path_correct(string const &target) {
    string cwd = filesystem::current_path();
    filesystem::path p(target);
    p.lexically_normal();

    string req_abs_path = filesystem::absolute(p);
    if (req_abs_path.back() == '/') {   // That cannot be regular file.
        return false;
    }

    if (req_abs_path.find(cwd) == 0) {  // Cwd needs to be a subpath.
        try {
            if(filesystem::exists(p) && !filesystem::is_regular_file(p)) {
                return false;
            }
        } catch (...) {                 // Path may be too long.
            return false;
        }
        return true;
    }

    return false;
}

// We need to check it each time since file may be modified.
void findInCorelated(string const &req_target) {
    string cur_line;
    bool is_found = false;

    filesystem::path p(req_target);
    p.lexically_normal();
    string req_abs_path = filesystem::absolute(p);

    fstream file;
    file.open(corelated, ios::in);
    if (!file.is_open()) {
        syserr("Cannot open correlated servers file");
    } else {
        while (getline(file, cur_line) && !is_found) {
            regex tab("\t");
            sregex_token_iterator it{cur_line.begin(), cur_line.end(), tab, -1};
            vector<string> params{it, {}};               // Splitting into params.

            if (params[0] == req_abs_path) {
                is_found = true;
                sendStatus302(params[1], params[2], req_target);
            }
        }
    }
    file.close();

    if (!is_found) {
        sendStatus404();
    }
};

// Handle request itself. Validate more specific task conditions.
void handleReq(string const &method, string const&req_target) {
    if (req_target[0] != '/') {     // Request must start with '/'.
        sendStatus400("Request target not starting with '/' ");
        return;
    }

    filesystem::path p(folder + req_target);
    string target = p.lexically_normal();
    if (!is_path_correct(target)) {
        sendStatus404();
        return;
    }

    // Try to open given request target path.
    int fd;
    if ((fd = open(target.c_str(), O_RDONLY)) == -1) {
        if (errno == EACCES || errno == ENOENT) { // Does not exist or cannot be opened.
            findInCorelated(req_target);          // We look for an unchanged request target.
        } else {                                  // Internal error while opening the file.
            sendStatus500();
        }
    } else {                                      // So we send file to our client.
        struct stat st;
        stat(target.c_str(), &st);
        size_t file_size = st.st_size;

        sendStatus200(file_size);

        if (method == "GET") {
            ssize_t cur_written ,len;
            size_t total_sent = 0;
            char write_buffer[BUFFER_SIZE];
            do {
                if ((len = read(fd, write_buffer, sizeof(write_buffer))) < 0) {
                    sendStatus500();
                    syserr("Writing to buffer");
                }

                // Client may end the connection.
                if ((cur_written = send(msg_sock, write_buffer, len, MSG_NOSIGNAL)) != len) {
                    conn_close = true;
                    return;
                }
                total_sent += cur_written;
            } while (total_sent != file_size);
        }

        close(fd);
    }
}

// Return true when parsed request is potentially correct (not validating path).
bool parseReqLine(string const &req_line, string &method, string &req_target) {
    regex reg(REQ_LINE);
    smatch req_match;

    regex sp(" ");
    sregex_token_iterator it{req_line.begin(), req_line.end(), sp, -1};
    vector<string> params{it, {}};               // Splitting into params.

    method = params[0];
    req_target = params[1];

    if (!regex_match(req_line, req_match, reg)) {// No request line match.
        return false;
    }

    return true;
}

// Return true when headers section is correct. Ignore unknown headers.
bool parseHeaderField(vector<string> &lines) {
    regex reg(":");
    string field_name, field_value, cur_line;

    for (size_t i = 1; i < lines.size() - 1; ++i) {
        sregex_token_iterator it{lines[i].begin(), lines[i].end(), reg, -1};
        vector<string> params{it, {}};

        if (params.size() != 2) {
            return false;
        }

        // Transforming to uppercase version.
        field_name = params[0];
        transform(field_name.begin(), field_name.end(), field_name.begin(), ::toupper);

        // Removing spaces.
        field_value = params[1];
        string::iterator end_pos = remove(field_value.begin(), field_value.end(), ' ');
        field_value.erase(end_pos, field_value.end());

        if (field_name.empty()) {
            return false;
        }
        // Actually, only Connection and Content-length headers should not be ignored.
        if (field_name == "CONNECTION") {
            if (field_value == "close") {
                if (conn_close) {       // Multiple headers Connection: close.
                    return false;
                }
                conn_close = true;
                continue;
            } else {                    // Field value different from (close).
                return false;
            }
        }

        // This is forbidden header field name.
        if (field_name == "CONTENT-LENGTH") {
            return false;
        }
    }

    return true;
}

// Handle and in particular validate http message in general format.
void handleMsg(string const &msg, string &method, string &req_target) {
    regex reg(CRLF);
    regex path(PATH);
    smatch path_match;
    sregex_token_iterator it{msg.begin(), msg.end(), reg, -1};
    vector<string> lines{it, {}};                       // Splitting into lines.

    if (!parseReqLine(lines[0], method, req_target)) {  // Check for correctness. Return request target.
        if (method != "GET" && method != "HEAD") {
            sendStatus501();
            return;
        } else {
            sendStatus400("Incorrect request line");
            return;
        }
    }

    if (!regex_match(req_target, path_match, path)) {   // Invalid path.
        sendStatus404();
        return;
    }

    if (!parseHeaderField(lines)) {                     // Invalid headers section.
        sendStatus400("Incorrect headers field");
    }
}

// Checking whether corelated servers file may be used,
void check_corelated() {
    fstream cor_file;
    cor_file.open(corelated, ios::in);
    if (!cor_file.is_open()) {
        syserr("Cannot find or open correlated servers file");
    }
    cor_file.close();
}

int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 4) {
        fatal("Usage: %s <folder> <corelated_servers> [<port>]", argv[0]);
    }

    int port = argc == 3 ? 8080 : stoi(argv[3]);

    // Normalising given parameters.
    filesystem::path p1(argv[1]);
    filesystem::path p2(argv[2]);
    folder = absolute(p1.lexically_normal());
    corelated = absolute(p2.lexically_normal());

    // Changing current working directory to given server-files directory,
    // in case one does not exist or cannot be opened we have EXIT_FAILURE.
    try {
        filesystem::current_path(p1);
    } catch (...) {
        fatal("Cannot find or open given directory");
    }

    check_corelated();


    int sock;
    ssize_t len = 0;
    char sock_buff[BUFFER_SIZE];
    socklen_t client_address_len;
    struct sockaddr_in server_address;
    struct sockaddr_in client_address;
    struct pollfd timeout_control;

    // Creating IPv4 TCP socket.
    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        syserr("socket");
    }

    server_address.sin_family = AF_INET;                    // IPv4
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);     // listening on all interfaces
    server_address.sin_port = htons(port);                  // listening on given port

    // Bind the socket to a concrete address.
    if (bind(sock, (struct sockaddr *) &server_address, sizeof(server_address)) < 0) {
        syserr("bind");
    }

    // Switch to listening (passive open).
    if (listen(sock, QUEUE_LENGTH) < 0) {
        syserr("listen");
    }

    cout << "Accepting client connections on port: "
         << ntohs(server_address.sin_port) << endl;
    cout << "with folder: " << folder << " and corelated servers in: "
         << corelated << endl;


    string data_buff;

    for (;;) {
        client_address_len = sizeof(client_address);
        msg_sock = accept(sock, (struct sockaddr *) &client_address, &client_address_len);
        if (msg_sock < 0) {
            sendStatus500();
            syserr("accept");
        }

        timeout_control.fd = msg_sock;
        timeout_control.events = POLLIN;

        data_buff.clear();
        conn_close = false;

        do {
            int ret = poll(&timeout_control, 1, TIMEOUT); // Timeout deaf connections.
            switch (ret) {
                case -1:                                  // Poll error.
                    sendStatus500();
                    syserr("poll error");
                    break;
                case 0:                                   // Timeout.
                    sendStatus400("Connection timeout");
                    break;
                default:
                    len = recv(msg_sock, sock_buff, sizeof(sock_buff), MSG_NOSIGNAL);
                    if (len < 0) {
                        sendStatus500();
                        syserr("reading from client socket");
                    } else {
                        printf("read from socket: %zd bytes: %.*s\n", len,
                               (int) len, sock_buff);
                    }

                    data_buff.append(sock_buff, len);

                    string method, req_target;
                    regex reg("\r\n\r\n");
                    smatch msg_match;

                    // Search for general http message pattern.
                    while (regex_search(data_buff, msg_match, reg) && !conn_close) {
                        res_send = false;
                        string cur_msg = data_buff.substr(0, msg_match.position(0) + 4);
                        data_buff.erase(0, cur_msg.size()); // Erasing current message.

                        handleMsg(cur_msg, method, req_target);

                        if (!res_send) handleReq(method, req_target);
                    }

                    if (data_buff.size() > MAX_SIZE) {      // Buffer max size exceeded.
                        data_buff.clear();
                        sendStatus500();
                    }
            }
        } while (len > 0 && !conn_close);

        cout << "Ending connection" << endl;
        if (close(msg_sock) < 0) {
            syserr("close");
        }
    }

    return 0;
}
