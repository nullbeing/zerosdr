#ifndef READSB_CONTROLLER_H
#define READSB_CONTROLLER_H

#include <string>
#include <sys/types.h>

/**
 * READSBController - Manages readsb subprocess lifecycle
 *
 * Launches readsb as a child process to decode ADSB messages.
 * Handles process creation, monitoring, and termination.
 */
class READSBController {
public:
    READSBController(const std::string& readsb_path,
                     const std::string& json_output_dir);
    ~READSBController();

    /**
     * Start readsb subprocess
     * @param gain Gain value in dB (0-49)
     * @return true if started successfully
     */
    bool start(int gain);

    /**
     * Stop readsb subprocess
     */
    void stop();

    /**
     * Check if readsb is running
     * @return true if process is alive
     */
    bool isRunning() const;

    /**
     * Get path to aircraft.json file
     * @return Full path to JSON output f  */
    std::string getJSONPath() const;

private:
    std::string readsb_path_;      // Path to readsb executable
    std::string json_output_dir_;  // Output directory for JSON files
    pid_t readsb_pid_;             // Process ID of readsb
    bool running_;                 // Running state flag

    bool createOutputDirectory();
};

#endif // READSB_CONTROLLER_H
