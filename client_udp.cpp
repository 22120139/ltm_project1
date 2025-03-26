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
#include <openssl/sha.h>  // Thư viện OpenSSL để tính checksum
#include <zlib.h>  // Thư viện hỗ trợ CRC32

#define PORT 8080
#define SERVER_IP "127.0.0.1"
#define CHUNK_SIZE 65536        // 64 KB
#define PAYLOAD_SIZE 1400       // UDP payload an toàn (MTU 1500 - header)
#define NUM_CONNECTIONS 4
#define RETRY_LIMIT 5

std::mutex file_mutex;
std::mutex progress_mutex;
long total_downloaded = 0;
long file_total_size = 1;  // Tránh chia cho 0

std::string calculate_checksum(const char *data, size_t len) {
    uLong crc = crc32(0L, Z_NULL, 0);  // Khởi tạo CRC
    crc = crc32(crc, reinterpret_cast<const Bytef *>(data), len);

    return std::string(reinterpret_cast<const char *>(&crc), sizeof(crc)); // 4 byte CRC32
}

void download_chunk(const std::string &filename, long start_offset, long end_offset, int thread_id) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return;
    }

    sockaddr_in server_addr = {AF_INET, htons(PORT)};
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    std::ofstream file(filename + ".part" + std::to_string(thread_id), std::ios::binary);
    char buffer[PAYLOAD_SIZE + 12]; // 8 bytes offset + 4 bytes checksum
    long received_bytes = 0;

    long send_size = std::min((long)PAYLOAD_SIZE, end_offset - start_offset);
    for (long offset = start_offset; offset < end_offset; offset += send_size) {
        send_size = std::min((long)PAYLOAD_SIZE, end_offset - offset);
        int retries = 0;
        bool success = false;

        while (retries < RETRY_LIMIT) {
            // Gửi yêu cầu tải chunk
            std::ostringstream request;
            request << "DOWNLOAD " << filename << " " << offset << " " << send_size;

            std::cout << "[Thread " << thread_id << "] Requesting chunk at offset: " << offset 
                      << " (size: " << send_size << " bytes)\n";

            ssize_t sent_bytes = sendto(sock, request.str().c_str(), request.str().size(), 0, 
                                        (struct sockaddr *)&server_addr, sizeof(server_addr));

            if (sent_bytes < 0) {
                perror("[Error] sendto failed");
                retries++;
                continue;
            }

            // Chờ nhận dữ liệu
            struct timeval timeout = {2, 0};  // 2 giây timeout
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

            sockaddr_in from_addr;
            socklen_t addr_len = sizeof(from_addr);
            ssize_t recv_bytes = recvfrom(sock, buffer, sizeof(buffer), 0, 
                                          (struct sockaddr *)&from_addr, &addr_len);

            if (recv_bytes < 12) {  // Gói tin quá nhỏ, có thể bị lỗi
                std::cerr << "[Error] Không nhận được dữ liệu hoặc gói tin quá nhỏ, thử lại...\n";
                retries++;
                continue;
            }

            // Đọc offset từ gói tin
            long received_offset;
            memcpy(&received_offset, buffer, sizeof(long));

            if (received_offset != offset) {
                std::cerr << "[Warning] Offset không khớp! Nhận " << received_offset 
                          << " nhưng mong đợi " << offset << ". Thử lại...\n";
                retries++;
                continue;
            }

            // Đọc checksum
            std::string received_checksum(buffer + 8, 4);
            std::string computed_checksum = calculate_checksum(buffer + 12, recv_bytes - 12);

            if (memcmp(received_checksum.data(), computed_checksum.data(), 4) != 0) {
                std::cerr << "[Error] Checksum không khớp! Thử lại...\n";
                retries++;
                continue;
            }

            // Ghi dữ liệu vào file (bỏ qua phần header 12 byte)
            file.write(buffer + 12, recv_bytes - 12);
            received_bytes += (recv_bytes - 12);

            // Gửi ACK sau khi kiểm tra dữ liệu hợp lệ
            std::cout << "[DEBUG] Gửi ACK cho offset: " << offset << "\n";
            ssize_t ack_sent = sendto(sock, &offset, sizeof(long), 0, 
                                      (struct sockaddr *)&server_addr, sizeof(server_addr));

            if (ack_sent < 0) {
                perror("[Error] sendto ACK failed");
            }

            // Cập nhật tiến trình tải
            std::lock_guard<std::mutex> lock(progress_mutex);
            total_downloaded += (recv_bytes - 12);
            double progress = std::min(100.0, (total_downloaded * 100.0) / file_total_size);
            std::cout << "\r[Progress] Downloading: " << progress << "%  " << std::flush;

            success = true;
            break; // Chunk đã tải xong, thoát vòng lặp retry
        }

        if (!success) {
            std::cerr << "[Thread " << thread_id << "] Lỗi tải chunk " << offset << " sau " 
                      << RETRY_LIMIT << " lần thử.\n";
        }
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
