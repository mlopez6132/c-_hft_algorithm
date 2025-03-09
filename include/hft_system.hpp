#ifndef HFT_SYSTEM_HPP
#define HFT_SYSTEM_HPP

#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <vector>
#include <chrono>

typedef websocketpp::client<websocketpp::config::asio_client> WebSocketClient;
using json = nlohmann::json;
using namespace std::chrono;

namespace hft {
    class Config;

    struct OrderBook {
        std::vector<std::pair<double, double>> bids;
        std::vector<std::pair<double, double>> asks;
        int64_t timestamp;
    };

    struct Order {
        std::string id;
        double price;
        double quantity;
        bool is_buy;
        system_clock::time_point timestamp;
    };

    class HFTSystem {
    public:
        HFTSystem(const std::string& config_file);
        void run();
        void stop();

    private:
        std::shared_ptr<spdlog::logger> logger_;
        Config config_;
        WebSocketClient ws_client_;
        boost::asio::io_context io_context_;
        websocketpp::connection_hdl ws_handle_;

        std::mutex order_book_mutex_;
        std::mutex position_mutex_;
        std::condition_variable cv_;
        std::queue<Order> order_queue_;
        OrderBook order_book_;

        std::atomic<bool> running_;
        std::atomic<double> current_position_;
        std::atomic<int64_t> last_trade_time_;

        struct Metrics {
            std::atomic<int64_t> trades_executed{0};
            std::atomic<double> latency_sum{0.0};
            std::atomic<int64_t> latency_count{0};
        } metrics_;

        void setup_handlers();
        void process_market_data(const std::string& payload);
        void evaluate_trading_opportunity();
        void execute_market_making(double bid, double ask);
        void simulate_order_execution(const Order& order);
        std::string generate_order_id();
        void monitor_system();
    };
}

#endif // HFT_SYSTEM_HPP
