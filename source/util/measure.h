#include <atomic>
#include <chrono>
#include <cstdio> 

class measure_t {
private:
    std::atomic_int counter;
    std::chrono::time_point<std::chrono::system_clock> last_time;

public:
    measure_t() {
        counter = 0;
        last_time = std::chrono::system_clock::now();
    }
    void increase() {
        ++counter;
        std::chrono::time_point<std::chrono::system_clock> current_time = std::chrono::system_clock::now();
        std::chrono::duration<double> interval = current_time - last_time; 
        if (interval.count() > 1.0) {
            printf ("\nThroughput: %d / sec\n", counter.load());
            counter = 0;
            last_time = current_time;
        }
    }
};