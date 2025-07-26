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
#include <openssl/sha.h>
#include <zlib.h>
#include <cstdio>
#include <sys/stat.h>
#include <csignal>
#include <set>
#include <iomanip>

#define PORT 8080
#define PAYLOAD_SIZE 1024
#define NUM_CONNECTIONS 4
#define INITIAL_RETRY_DELAY_MS 100  // 100ms cho lần retry đầu tiên
#define MAX_RETRY_DELAY_MS 2000     // Tối đa 2s giữa các lần retry
#define MAX_RETRIES 30  

std::mutex file_mutex;
std::mutex progress_mutex;
long file_total_size = 1;
long total_downloaded = 0;
std::vector<long> chunk_downloaded(NUM_CONNECTIONS, 0);
std::vector<long> chunk_size(NUM_CONNECTIONS, 0);


std::string calculate_checksum(const char *data, size_t len) {
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef *>(data), len);
    return std::string(reinterpret_cast<const char *>(&crc), sizeof(crc));
}

bool retry_with_backoff(int sock, const std::string& request, 
                       const sockaddr_in& server_addr,
                       char* buffer, size_t buffer_size,
                       long expected_offset, int& retry_count,
                       ssize_t& received_bytes) {  // Thêm tham số tham chiếu để trả về số byte nhận được
    int delay_ms = INITIAL_RETRY_DELAY_MS;
    
    while (retry_count < MAX_RETRIES) {
        // Gửi request
        if (sendto(sock, request.c_str(), request.size(), 0,
                  (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            perror("[Error] sendto failed");
            retry_count++;
            continue;
        }

        // Thiết lập timeout
        struct timeval timeout;
        timeout.tv_sec = delay_ms / 1000;
        timeout.tv_usec = (delay_ms % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        // Nhận phản hồi
        sockaddr_in from_addr;
        socklen_t addr_len = sizeof(from_addr);
        received_bytes = recvfrom(sock, buffer, buffer_size, 0,
                                (struct sockaddr *)&from_addr, &addr_len);

        if (received_bytes < 12) {
            retry_count++;
            delay_ms = std::min(delay_ms * 2, MAX_RETRY_DELAY_MS);
            continue;
        }

        // Kiểm tra offset
        long received_offset;
        memcpy(&received_offset, buffer, sizeof(long));
        if (received_offset != expected_offset) {
            retry_count++;
            delay_ms = std::min(delay_ms * 2, MAX_RETRY_DELAY_MS);
            continue;
        }

        // Kiểm tra checksum
        std::string received_checksum(buffer + 8, 4);
        std::string computed_checksum = calculate_checksum(buffer + 12, received_bytes - 12);
        if (memcmp(received_checksum.data(), computed_checksum.data(), 4) != 0) {
            retry_count++;
            delay_ms = std::min(delay_ms * 2, MAX_RETRY_DELAY_MS);
            continue;
        }

        return true; // Thành công
    }
    
    return false; // Thất bại sau tất cả retry
}

void download_chunk(const std::string &filename, long start_offset, long end_offset, 
                   int thread_id, const char* server_ip) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return;
    }
    

    sockaddr_in server_addr = {AF_INET, htons(PORT)};
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    std::string part_filename = filename + ".part" + std::to_string(thread_id);
    std::ofstream file(part_filename, std::ios::binary);
    char buffer[PAYLOAD_SIZE + 12];

    for (long offset = start_offset; offset < end_offset;) {
        long send_size = std::min((long)PAYLOAD_SIZE, end_offset - offset);
        int retry_count = 0;
        ssize_t recv_bytes = 0;

        std::ostringstream request;
        request << "DOWNLOAD " << filename << " " << offset << " " << send_size 
                << " " << thread_id;

        if (retry_with_backoff(sock, request.str(), server_addr, buffer, sizeof(buffer), 
                              offset, retry_count, recv_bytes)) {

            file.write(buffer + 12, recv_bytes - 12);

            // Gửi ACK
           std::ostringstream oss;
            oss << "ACK " << offset << " " << thread_id;

            std::string ack_msg = oss.str();

            ssize_t ack_sent = sendto(sock, ack_msg.c_str(), ack_msg.size(), 0,
                                    (struct sockaddr *)&server_addr, sizeof(server_addr));

            // Cập nhật tiến trình
            {
                std::lock_guard<std::mutex> lock(progress_mutex);
                chunk_downloaded[thread_id] += (recv_bytes - 12); // chỉ cộng dữ liệu thật
                double percent = (chunk_downloaded[thread_id] * 100.0) / chunk_size[thread_id];

                std::cout << "\r";
                for (int j = 0; j < NUM_CONNECTIONS; ++j) {
                    double p = (chunk_downloaded[j] * 100.0) / chunk_size[j];
                    std::cout << "[Part " << j << ": " << std::setw(6) << std::fixed << std::setprecision(2) << p << "%] ";
                }
                std::cout << std::flush;
            }


            offset += send_size;
        } else {
                    // Ghi lại chunk lỗi
                    std::ofstream log("failed_chunks_" + filename + ".log", std::ios::app);
                    log << offset << " " << send_size << "\n";
                    log.close();

                    offset += send_size; // Bỏ qua chunk lỗi
                }

    }

    file.close();
    close(sock);
}

bool file_exists(const std::string &filename) {
    struct stat buffer;
    return (stat(filename.c_str(), &buffer) == 0);
}

std::string get_unique_filename(const std::string &filename) {
    std::string unique_filename = filename + "_download";
    int index = 1;

    while (file_exists(unique_filename)) {
        unique_filename = filename + "_download_" + std::to_string(index);
        index++;
    }

    return unique_filename;
}

void merge_file(const std::string &filename) {
    std::string merged_filename = get_unique_filename(filename);
    std::ofstream outfile(merged_filename, std::ios::binary);

    if (!outfile) {
        std::cerr << "[Error] Không thể tạo file merge: " << merged_filename << std::endl;
        return;
    }

    // Merge các phần chính
    for (int i = 0; i < NUM_CONNECTIONS; i++) {
        std::string part_filename = filename + ".part" + std::to_string(i);
        std::ifstream infile(part_filename, std::ios::binary);

        if (!infile) {
            std::cerr << "[Error] Không tìm thấy file " << part_filename << std::endl;
            continue;
        }

        outfile << infile.rdbuf();
        infile.close();
        remove(part_filename.c_str());
    }

    // Merge phần tải lại (nếu có)
    std::string retry_part = filename + ".part999";
    std::ifstream retry_file(retry_part, std::ios::binary);
    if (retry_file) {
        std::cout << "[Info] Đang merge phần tải lại: " << retry_part << std::endl;
        outfile << retry_file.rdbuf();
        retry_file.close();
        remove(retry_part.c_str());
    }

    outfile.close();
    std::cout << "[Info] Đã merge file: " << merged_filename << std::endl;
}

void download_file(const std::string &filename, long file_size, const char* server_ip) {
    file_total_size = file_size;
    long chunk_per_thread = file_size / NUM_CONNECTIONS;
    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_CONNECTIONS; i++) {
        long start_offset = i * chunk_per_thread;
        long end_offset = (i == NUM_CONNECTIONS - 1) ? file_size : start_offset + chunk_per_thread;
        chunk_size[i] = end_offset - start_offset;
        threads.emplace_back(download_chunk, filename, start_offset, end_offset, i, server_ip);
    }

    for (auto &t : threads) t.join();

    // After all threads complete and before merging:
    // Ensure the progress line is cleared before printing new messages.
    {
        std::lock_guard<std::mutex> lock(progress_mutex);
        std::cout << "\r" << std::string(80, ' ') << "\r"; // Clear the current line (e.g., 80 spaces)
        std::cout.flush();
    }

    // Sau merge_file(filename);
    std::ifstream failed_log("failed_chunks_" + filename + ".log");
    if (failed_log) {
        std::cout << "[!] Phát hiện phần bị lỗi. Bạn có muốn thử tải lại? (y/n): ";
        char ch;
        std::cin >> ch;
        if (ch == 'y' || ch == 'Y') {
            long offset, size;
            while (failed_log >> offset >> size) {
                // Clear the line again before starting retry download progress
                {
                    std::lock_guard<std::mutex> lock(progress_mutex);
                    std::cout << "\r" << std::string(80, ' ') << "\r";
                    std::cout.flush();
                }
                download_chunk(filename, offset, offset + size, 999, server_ip); // 999 là thread_id tạm
            }
            failed_log.close();
            remove(("failed_chunks_" + filename + ".log").c_str());
            
            // Clear the line once more after retry attempts, before merging again
            {
                std::lock_guard<std::mutex> lock(progress_mutex);
                std::cout << "\r" << std::string(80, ' ') << "\r";
                std::cout.flush();
            }
            merge_file(filename); // Merge lại
        }
    }
    else {
            std::cout << "[Info] Hoàn tất tải. Đang tiến hành merge...\n";
            merge_file(filename);
    }
    total_downloaded = 0;
}

void request_file_list(const char* server_ip) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("[ERROR] Socket creation failed");
        return;
    }

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

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

long get_file_size(const std::string &filename, const char* server_ip) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("[ERROR] Socket creation failed");
        return -1;
    }

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

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


std::vector<std::string> readFromLine(size_t skipLines, const std::string& filename) {
    std::vector<std::string> result;
    std::ifstream file(filename);

    if (!file.is_open()) {
        std::cerr << "Không thể mở file: " << filename << std::endl;
        return result;
    }

    std::string line;
    size_t currentLine = 0;

    while (std::getline(file, line)) {
        if (currentLine >= skipLines) {
            result.push_back(line); // Thêm dòng sau khi bỏ qua skipLines dòng đầu tiên
        }
        currentLine++;
    }

    file.close();
    return result;
}

void display_menu() {
    std::cout << "\n===== MENU =====" << std::endl;
    std::cout << "1. Xem danh sách file trên server" << std::endl;
    std::cout << "2. Thêm và tải file ngay lập tức" << std::endl;
    std::cout << "3. Thoát chương trình" << std::endl;
    std::cout << "Lựa chọn của bạn: ";
}

void menu(const char* server_ip) {
    std::string filename = "input.txt";
    size_t processedLines = 0; // Theo dõi số dòng đã xử lý
    int choice;

    do {
        display_menu();
        std::cin >> choice;
        std::cin.ignore(); // Xóa bộ đệm nhập

        switch (choice) {
            case 1:
                request_file_list(server_ip);
                break;
            case 2: {
                while (true) {
                    // Đọc các dòng mới từ file
                    std::vector<std::string> newFiles = readFromLine(processedLines, filename);

                    if (newFiles.empty()) {
                        break;
                    }

                    for (const auto& file : newFiles) {
                        long file_size = get_file_size(file, server_ip);
                        if (file_size > 0) {
                            download_file(file, file_size, server_ip);
                            std::cout << "Đã tải xong file: " << file << "\n";
                        } else {
                            std::cerr << "[ERROR] Không thể tải file: " << file << std::endl;
                        }
                    }

                    processedLines += newFiles.size();
                }

                break;
            }
            case 3:
                std::cout << "Thoát chương trình..." << std::endl;
                break;
            default:
                std::cout << "Lựa chọn không hợp lệ, vui lòng thử lại!" << std::endl;
        }
    } while (choice != 3);
}

// Hàm xử lý tín hiệu Ctrl + C
void signal_handler(int signal) {
    std::cout << "\n[INFO] Nhận tín hiệu Ctrl + C. Đang thoát chương trình...\n";
    exit(0);
}

int main(int argc, char *argv[]) {
    // Đăng ký bắt tín hiệu Ctrl + C
    signal(SIGINT, signal_handler);

    // Kiểm tra tham số dòng lệnh
    if (argc < 2) {
        printf("Cách dùng: %s <Server_IP>\n", argv[0]);
        return 1;
    }

    char *server_ip = argv[1]; // Lấy IP từ tham số dòng lệnh
    printf("Server đang chạy trên IP: %s, Port: %d\n", server_ip, PORT);

    menu(server_ip);
    return 0;
}