#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <thread>
#include <mutex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sys/time.h>

#define PORT 8080
#define SERVER_IP "127.0.0.1"
#define CHUNK_SIZE 65536        // 64 KB
#define PAYLOAD_SIZE 1400       // UDP payload an toàn (MTU 1500 - header)
#define NUM_CONNECTIONS 4

std::mutex file_mutex;
std::mutex progress_mutex;
long total_downloaded = 0;
long file_total_size = 1;  // Tránh chia cho 0

void download_chunk(const std::string &filename, long start_offset, long end_offset, int thread_id) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return;
    }

    sockaddr_in server_addr = {AF_INET, htons(PORT)};
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    std::ofstream file(filename + ".part" + std::to_string(thread_id), std::ios::binary);
    char buffer[PAYLOAD_SIZE + 8]; // 8 bytes header chứa offset
    long received_bytes = 0;

    long send_size = std::min((long)PAYLOAD_SIZE, end_offset - start_offset); // Xác định kích thước trước
    for (long offset = start_offset; offset < end_offset; offset += send_size) {
        send_size = std::min((long)PAYLOAD_SIZE, end_offset - offset);

        std::ostringstream request;
        request << "DOWNLOAD " << filename << " " << offset << " " << send_size;

        std::cout << "[Thread " << thread_id << "] Requesting: " << filename
                  << " from offset " << offset << " (chunk size: " << send_size << " bytes)" << std::endl;

        ssize_t sent_bytes = sendto(sock, request.str().c_str(), request.str().size(), 0, 
                                    (struct sockaddr *)&server_addr, sizeof(server_addr));

        if (sent_bytes < 0) {
            perror("[Error] sendto failed");
            continue;
        }

        // Nhận dữ liệu
        ssize_t recv_bytes = recvfrom(sock, buffer, sizeof(buffer), 0, NULL, NULL);
        if (recv_bytes <= 8) {
            std::cerr << "[Error] Không nhận được dữ liệu hoặc gói tin quá nhỏ\n";
            continue;
        }

        // Đọc offset từ gói tin
        long received_offset;
        memcpy(&received_offset, buffer, sizeof(long));

        if (received_offset != offset) {
            std::cerr << "[Warning] Offset không khớp! Bỏ qua gói tin.\n";
            continue;
        }

        // Ghi dữ liệu vào file (bỏ qua phần header 8 byte)
        file.write(buffer + 8, recv_bytes - 8);
        received_bytes += (recv_bytes - 8);

        std::lock_guard<std::mutex> lock(progress_mutex);
        total_downloaded += (recv_bytes - 8);
        double progress = std::min(100.0, (total_downloaded * 100.0) / file_total_size);
        std::cout << "\r[Progress] Downloading: " << progress << "%  " << std::flush;
    }

    file.close();
    close(sock);

    std::cout << "[Thread " << thread_id << "] Finished downloading " 
              << filename << " part " << thread_id 
              << " (" << received_bytes << " bytes)" << std::endl;
}


void merge_file(const std::string &filename) {
    std::ofstream outfile(filename + "_merged", std::ios::binary);
    if (!outfile) {
        std::cerr << "Error creating merged file" << std::endl;
        return;
    }

    for (int i = 0; i < NUM_CONNECTIONS; i++) {
        std::string part_filename = filename + ".part" + std::to_string(i);
        std::ifstream infile(part_filename, std::ios::binary | std::ios::ate);

        if (!infile) {
            std::cerr << "[Error] Không tìm thấy file " << part_filename << std::endl;
            continue;
        }

        std::streamsize size = infile.tellg(); // Lấy kích thước file
        infile.seekg(0, std::ios::beg);        // Quay lại đầu file để đọc

        if (size == 0) {
            std::cerr << "[Warning] File " << part_filename << " có kích thước 0!" << std::endl;
            infile.close();
            continue;
        }

        std::cout << "[Debug] Đọc file " << part_filename << " (kích thước: " << size << " bytes)" << std::endl;
        outfile << infile.rdbuf(); // Ghép nội dung vào file gốc
        infile.close();

        if (remove(part_filename.c_str()) == 0) {
            std::cout << "[Debug] Xóa file " << part_filename << " sau khi merge thành công." << std::endl;
        } else {
            std::cerr << "[Error] Không thể xóa file " << part_filename << std::endl;
        }
    }

    outfile.close();
    std::cout << "[Debug] File merge hoàn tất: " << filename + "_merged" << std::endl;
}

void download_file(const std::string &filename, long file_size) {
    file_total_size = file_size;  // Gán kích thước tổng

    long chunk_per_thread = file_size / NUM_CONNECTIONS;  // Mỗi thread xử lý phần này
    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_CONNECTIONS; i++) {
        long start_offset = i * chunk_per_thread;
        long end_offset = (i == NUM_CONNECTIONS - 1) ? file_size : start_offset + chunk_per_thread;

        // Tạo thread tải phần của file (gồm nhiều chunk nhỏ)
        threads.emplace_back(download_chunk, filename, start_offset, end_offset, i);
    }

    for (auto &t : threads) t.join();

    std::cout << "[Debug] Tất cả luồng đã kết thúc, chuẩn bị merge file..." << std::endl;
    merge_file(filename);
}

void request_file_list() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("[ERROR] Socket creation failed");
        return;
    }

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    std::string request = "LIST";
    sendto(sock, request.c_str(), request.size(), 0, (struct sockaddr *)&server_addr, sizeof(server_addr));

    char buffer[4096];
    ssize_t recv_bytes = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, NULL, NULL);
    if (recv_bytes > 0) {
        buffer[recv_bytes] = '\0';
        std::cout << "[File List]\n" << buffer;
    } else {
        std::cerr << "[ERROR] Failed to receive file list" << std::endl;
    }

    close(sock);
}

long get_file_size(const std::string &filename) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("[ERROR] Socket creation failed");
        return -1;
    }

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    std::string request = "SIZE " + filename;
    sendto(sock, request.c_str(), request.size(), 0, (struct sockaddr *)&server_addr, sizeof(server_addr));

    char buffer[64];
    ssize_t recv_bytes = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, NULL, NULL);
    close(sock);

    if (recv_bytes <= 0) {
        std::cerr << "[ERROR] Không thể lấy kích thước file từ server\n";
        return -1;
    }

    buffer[recv_bytes] = '\0';
    return std::stol(buffer); // Chuyển đổi kích thước file từ string sang long
}

int main() {
    request_file_list();  // Yêu cầu danh sách file từ server
    std::ifstream input_file("input.txt");
    std::string filename;
    
    while (std::getline(input_file, filename)) {
        long file_size = get_file_size(filename);
        if (file_size > 0) {
            download_file(filename, file_size);
        } else {
            std::cerr << "[ERROR] Không thể tải file: " << filename << std::endl;
        }
    }
}
