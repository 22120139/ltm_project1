#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <arpa/inet.h>
#include <unistd.h>
#include <algorithm>
#include <dirent.h>  // Thư viện để liệt kê file trong thư mục
#include <sys/stat.h>  // Để lấy kích thước file

#define PORT 8080
#define HEADER_SIZE 10  
#define MAX_CHUNK_SIZE 4096  

// Lấy kích thước file (trả về -1 nếu lỗi)
long get_file_size(const std::string &filename) {
    struct stat st;
    if (stat(filename.c_str(), &st) == 0)
        return st.st_size;
    return -1;
}

// Cập nhật danh sách file vào files.txt
void update_file_list() {
    DIR *dir = opendir(".");
    if (!dir) {
        perror("[ERROR] Không thể mở thư mục");
        return;
    }

    std::ofstream file_list("files.txt");  // Ghi đè nội dung cũ
    if (!file_list) {
        std::cerr << "[ERROR] Không thể ghi files.txt" << std::endl;
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {  // Chỉ lấy file thường
            std::string filename = entry->d_name;
            long size = get_file_size(filename);
            if (size > 0) {
                file_list << filename << " " << size / (1024 * 1024) << "MB\n";
            }
        }
    }
    closedir(dir);
    file_list.close();
}

std::string list_files() {
    std::ifstream file_list("files.txt");
    if (!file_list) {
        return "[ERROR] Không thể mở files.txt\n";
    }

    std::ostringstream response;
    std::string line;
    while (std::getline(file_list, line)) {
        response << line << "\n";
    }
    return response.str();
}

void send_file_list(int sock, sockaddr_in client_addr, socklen_t addr_len) {
    std::string file_list;
    
    // Mở thư mục hiện tại
    DIR *dir = opendir(".");
    if (!dir) {
        perror("[ERROR] Failed to open directory");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_REG) {  // Chỉ lấy file, bỏ qua thư mục
            file_list += entry->d_name;
            file_list += "\n";
        }
    }
    closedir(dir);

    if (file_list.empty()) {
        file_list = "No files available\n";
    }

    // Gửi danh sách file đến client
    sendto(sock, file_list.c_str(), file_list.size(), 0, (struct sockaddr *)&client_addr, addr_len);
    std::cout << "[INFO] Sent file list to client.\n";
}

void handle_client(int sock, sockaddr_in client_addr, socklen_t addr_len) {
    char request[1024];
    ssize_t recv_bytes = recvfrom(sock, request, sizeof(request) - 1, 0, (struct sockaddr *)&client_addr, &addr_len);
    
    if (recv_bytes <= 0) {
        std::cerr << "[ERROR] Failed to receive request\n";
        return;
    }
    request[recv_bytes] = '\0';

    // Parse request
    std::istringstream iss(request);
    std::string command;
    iss >> command;

    if (command == "LIST") {
        update_file_list();  // Cập nhật danh sách trước khi gửi
        std::string file_list = list_files();
        sendto(sock, file_list.c_str(), file_list.size(), 0, (struct sockaddr *)&client_addr, sizeof(client_addr));
    }

    else if (command == "DOWNLOAD") {
        std::string filename;
        long offset, requested_chunk_size;
        iss >> filename >> offset >> requested_chunk_size;

        // Mở file
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file) {
            std::cerr << "[ERROR] Failed to open file: " << filename << "\n";
            return;
        }

        // Xác định kích thước file
        long file_size = file.tellg();
        file.seekg(0, std::ios::beg);

        // Kiểm tra offset hợp lệ
        if (offset >= file_size) {
            std::cerr << "[ERROR] Offset " << offset << " exceeds file size " << file_size << "\n";
            return;
        }

        // Tính toán chunk size hợp lý
        long chunk_size = std::min({requested_chunk_size, file_size - offset, (long)MAX_CHUNK_SIZE});

        // Debug
        std::cout << "[DEBUG] File size: " << file_size << " bytes\n";
        std::cout << "[DEBUG] Offset requested: " << offset << "\n";
        std::cout << "[DEBUG] Chunk size: " << chunk_size << " bytes\n";

        // Cấp phát buffer động
        long buffer_size = chunk_size + sizeof(int64_t);
        char *buffer = new char[buffer_size];

        // Ghi offset vào đầu buffer
        memcpy(buffer, &offset, sizeof(int64_t));

        // Đọc dữ liệu từ file
        file.seekg(offset, std::ios::beg);
        file.read(buffer + sizeof(int64_t), chunk_size);
        long bytes_read = file.gcount();

        if (bytes_read <= 0) {
            std::cerr << "[ERROR] Failed to read file at offset " << offset << "\n";
            delete[] buffer;
            return;
        }

        std::cout << "[DEBUG] Sending " << bytes_read << " bytes to client\n";

        // Gửi dữ liệu tới client
        sendto(sock, buffer, bytes_read + sizeof(int64_t), 0, (struct sockaddr *)&client_addr, addr_len);

        // Giải phóng bộ nhớ
        delete[] buffer;
    } 

    else if (strncmp(request, "SIZE ", 5) == 0) {
        std::string filename = request + 5;  // Lấy tên file từ chuỗi request
        std::ifstream file(filename, std::ios::binary | std::ios::ate);

        if (!file) {
            std::string response = "-1";
            sendto(sock, response.c_str(), response.size(), 0, (struct sockaddr *)&client_addr, addr_len);
        } else {
            long size = file.tellg();
            std::string response = std::to_string(size);
            sendto(sock, response.c_str(), response.size(), 0, (struct sockaddr *)&client_addr, addr_len);
        }
    }

    else {
        std::cerr << "[ERROR] Invalid command: " << command << "\n";
    }
}

int main() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return EXIT_FAILURE;
    }

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(sock);
        return EXIT_FAILURE;
    }

    std::cout << "Server UDP listening on port " << PORT << "\n";

    while (true) {
        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        handle_client(sock, client_addr, addr_len);
    }

    close(sock);
    return 0;
}
