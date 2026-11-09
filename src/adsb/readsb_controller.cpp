#include "adsb/readsb_controller.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>

READSBController::READSBController(const std::string& readsb_path,
                                   const std::string& json_output_dir)
    : readsb_path_(readsb_path),
      json_output_dir_(json_output_dir),
      readsb_pid_(-1),
      running_(false) {
}

READSBController::~READSBController() {
    stop();
}

bool READSBController::createOutputDirectory() {
    // Create output directory if it doesn't exist
    struct stat st;
    if (stat(json_output_dir_.c_str(), &st) != 0) {
        if (mkdir(json_output_dir_.c_str(), 0755) != 0) {
            std::cerr << "Failed to create directory: " << json_output_dir_ << std::endl;
            return false;
        }
    }
    return true;
}

bool READSBController::start(int gain) {
    if (running_) {
        std::cout << "readsb already running" << std::endl;
        return true;
    }

    // Create output directory
    if (!createOutputDirectory()) {
        return false;
    }

    // Fork and exec readsb
    pid_t pid = fork();

    if (pid < 0) {
        std::cerr << "Failed to fork readsb process" << std::endl;
        return false;
    }

    if (pid == 0) {
        // Child process - exec readsb
        char gain_str[16];
        if (gain < 0) {
            snprintf(gain_str, sizeof(gain_str), "auto");
        } else {
            snprintf(gain_str, sizeof(gain_str), "%d", gain);
        }

        // Redirect stdout/stderr to a log file for debugging
        std::string log_path = json_output_dir_ + "readsb.log";
        freopen(log_path.c_str(), "w", stdout);
        freopen(log_path.c_str(), "a", stderr);

        execl(readsb_path_.c_str(),
              "readsb",
              "--device-type", "rtlsdr",
              "--gain", gain_str,
              "--write-json", json_output_dir_.c_str(),
              "--quiet",
    nullptr);

        // If execl returns, it failed
        std::cerr << "Failed to exec readsb: " << strerror(errno) << std::endl;
        _exit(1);
    }

    // Parent process
    readsb_pid_ = pid;
    running_ = true;

    std::cout << "Started readsb (PID: " << readsb_pid_ << ", gain: " << gain << " dB)" << std::endl;

    // Give readsb a moment to start
    usleep(500000);  // 500ms

    // Check if process is still alive
    if (!isRunning()) {
        std::cerr << "dsb process died immediately after start" << std::endl;
        running_ = false;
        readsb_pid_ = -1;
        return false;
    }

    return true;
}

void READSBController::stop() {
    if (!running_ || readsb_pid_ < 0) {
        return;
    }

    std::cout << "Stopping readsb (PID: " << readsb_pid_ << ")" << std::endl;

    // Send SIGTERM to gracefully terminate
    kill(readsb_pid_, SIGTERM);

    // Wait up to 2 seconds for process to exit
    for (int i = 0; i < 20; i++) {
        int status;
        pid_t result = waitpid(readsb_pid_, &status, WNOHANG);

        if (result == readsb_pid_) {
            // Process exited
            std::cout << "readsb terminated gracefully" << std::endl;
            running_ = false;
            readsb_pid_ = -1;
            return;
        }

        usleep(100000);  // 100ms
    }

    // If still running, force kill
    std::cout << "readsb did not terminate, sending SIGKILL" << std::endl;
    kill(readsb_pid_, SIGKILL);
    waitpid(readsb_pid_, nullptr, 0);

    running_ = false;
    readsb_pid_ = -1;
}

bool READSBController::isRunning() const {
    if (!running_ || readsb_pid_ < 0) {
        return false;
    }

    // Check if process exists without reaping it
    // kill(pid, 0) returns 0 if process exists, -1 if not
    return kill(readsb_pid_, 0) == 0;
}

std::string READSBController::getJSONPath() const {
    return json_output_dir_ + "aircraft.json";
}
