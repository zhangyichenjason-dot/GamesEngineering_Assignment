#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>
#include <thread>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

// 全局变量：存储用户名和对应的套接字
std::map<std::string, SOCKET> clients;
std::mutex clients_mutex;

// 消息类型
enum MessageType {
    BROADCAST = 1,  // 广播消息
    PRIVATE = 2,    // 私聊消息
    LOGIN = 3       // 登录消息
};

// 解析消息协议：Type|Target|Content
struct Message {
    int type;
    std::string target;
    std::string content;
    bool valid;

    Message() : type(0), valid(false) {}
};

Message parseMessage(const std::string& raw) {
    Message msg;
    std::istringstream iss(raw);
    std::string type_str, target, content;

    if (std::getline(iss, type_str, '|') &&
        std::getline(iss, target, '|') &&
        std::getline(iss, content)) {

        try {
            msg.type = std::stoi(type_str);
            msg.target = target;
            msg.content = content;
            msg.valid = true;
        }
        catch (...) {
            msg.valid = false;
        }
    }

    return msg;
}

// 发送消息给指定的socket
void sendMessage(SOCKET sock, const std::string& message) {
    send(sock, message.c_str(), static_cast<int>(message.size()), 0);
}

// 广播消息给所有客户端（除了发送者）
void broadcastMessage(const std::string& sender, const std::string& content) {
    std::lock_guard<std::mutex> lock(clients_mutex);

    std::string message = "[" + sender + "]: " + content;

    for (const auto& client : clients) {
        // 不发送给自己
        if (client.first != sender) {
            sendMessage(client.second, message);
        }
    }

    std::cout << "Broadcast from " << sender << ": " << content << std::endl;
}

// 发送私聊消息
void sendPrivateMessage(const std::string& sender, const std::string& target, const std::string& content) {
    std::lock_guard<std::mutex> lock(clients_mutex);

    auto it = clients.find(target);
    if (it != clients.end()) {
        std::string message = "[Private from " + sender + "]: " + content;
        sendMessage(it->second, message);
        std::cout << "Private message from " << sender << " to " << target << ": " << content << std::endl;
    }
    else {
        // 目标用户不存在，通知发送者
        auto sender_it = clients.find(sender);
        if (sender_it != clients.end()) {
            std::string error_msg = "[System]: User '" + target + "' not found.";
            sendMessage(sender_it->second, error_msg);
        }
        std::cout << "Failed: User " << target << " not found." << std::endl;
    }
}

// 发送在线用户列表
void sendUserList(SOCKET client_socket) {
    std::lock_guard<std::mutex> lock(clients_mutex);

    std::string user_list = "[System]: Online users: ";
    for (const auto& client : clients) {
        user_list += client.first + ", ";
    }
    if (!clients.empty()) {
        user_list = user_list.substr(0, user_list.length() - 2); // 移除最后的逗号和空格
    }

    sendMessage(client_socket, user_list);
}

// 处理客户端连接
void handleClient(SOCKET client_socket, int connection_id) {
    std::string username;
    bool logged_in = false;

    // 请求用户登录
    sendMessage(client_socket, "[System]: Please login with format: 3|Any|YourUsername");

    while (true) {
        char buffer[1024] = { 0 };
        int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);

        if (bytes_received <= 0) {
            // 连接断开
            break;
        }

        buffer[bytes_received] = '\0';
        std::string raw_message(buffer);

        std::cout << "Connection " << connection_id << " received: " << raw_message << std::endl;

        Message msg = parseMessage(raw_message);

        if (!msg.valid) {
            sendMessage(client_socket, "[System]: Invalid message format. Use: Type|Target|Content");
            continue;
        }

        // 处理登录
        if (msg.type == LOGIN) {
            if (logged_in) {
                sendMessage(client_socket, "[System]: Already logged in as " + username);
                continue;
            }

            username = msg.content;

            // 检查用户名是否已存在
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                if (clients.find(username) != clients.end()) {
                    sendMessage(client_socket, "[System]: Username already taken. Please choose another.");
                    continue;
                }

                // 添加到客户端列表
                clients[username] = client_socket;
            }

            logged_in = true;
            std::cout << "User '" << username << "' logged in (Connection " << connection_id << ")" << std::endl;

            sendMessage(client_socket, "[System]: Welcome " + username + "! You are now logged in.");
            sendUserList(client_socket);

            // 通知其他用户
            broadcastMessage("System", username + " has joined the chat.");

            continue;
        }

        // 未登录时不能发送其他消息
        if (!logged_in) {
            sendMessage(client_socket, "[System]: Please login first.");
            continue;
        }

        // 处理退出
        if (msg.content == "!bye" || msg.content == "!quit") {
            sendMessage(client_socket, "[System]: Goodbye!");
            break;
        }

        // 处理广播消息
        if (msg.type == BROADCAST) {
            broadcastMessage(username, msg.content);
        }
        // 处理私聊消息
        else if (msg.type == PRIVATE) {
            sendPrivateMessage(username, msg.target, msg.content);
        }
        else {
            sendMessage(client_socket, "[System]: Unknown message type. Use 1 for broadcast, 2 for private.");
        }
    }

    // 清理：移除用户
    if (logged_in) {
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            clients.erase(username);
        }

        std::cout << "User '" << username << "' disconnected (Connection " << connection_id << ")" << std::endl;
        broadcastMessage("System", username + " has left the chat.");
    }
    else {
        std::cout << "Connection " << connection_id << " disconnected without logging in." << std::endl;
    }

    closesocket(client_socket);
}

int main() {
    // 初始化 WinSock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed with error: " << WSAGetLastError() << std::endl;
        return 1;
    }

    // 创建服务器套接字
    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == INVALID_SOCKET) {
        std::cerr << "Socket creation failed with error: " << WSAGetLastError() << std::endl;
        WSACleanup();
        return 1;
    }

    // 绑定地址和端口
    sockaddr_in server_address = {};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(65432);
    server_address.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_socket, (sockaddr*)&server_address, sizeof(server_address)) == SOCKET_ERROR) {
        std::cerr << "Bind failed with error: " << WSAGetLastError() << std::endl;
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    // 监听连接
    if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Listen failed with error: " << WSAGetLastError() << std::endl;
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    std::cout << "=== Chat Room Server ===" << std::endl;
    std::cout << "Server is listening on port 65432..." << std::endl;
    std::cout << "Message Protocol: Type|Target|Content" << std::endl;
    std::cout << "  Type 1: Broadcast (Target='All')" << std::endl;
    std::cout << "  Type 2: Private (Target=Username)" << std::endl;
    std::cout << "  Type 3: Login (Content=Username)" << std::endl;
    std::cout << "========================" << std::endl << std::endl;

    // 接受客户端连接
    int connection_id = 0;
    std::vector<std::thread*> threads;

    while (true) {
        sockaddr_in client_address = {};
        int client_address_len = sizeof(client_address);
        SOCKET client_socket = accept(server_socket, (sockaddr*)&client_address, &client_address_len);

        if (client_socket == INVALID_SOCKET) {
            std::cerr << "Accept failed with error: " << WSAGetLastError() << std::endl;
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_address.sin_addr, client_ip, INET_ADDRSTRLEN);

        connection_id++;
        std::cout << "New connection from " << client_ip << ":" << ntohs(client_address.sin_port);
        std::cout << " (Connection ID: " << connection_id << ")" << std::endl;

        // 创建新线程处理客户端
        std::thread* t = new std::thread(handleClient, client_socket, connection_id);
        t->detach(); // 分离线程
        threads.push_back(t);
    }

    // 清理
    closesocket(server_socket);
    WSACleanup();

    return 0;
}