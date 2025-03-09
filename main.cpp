#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <optional>

typedef websocketpp::client<websocketpp::config::asio_client> WebSocketClient;
using json = nlohmann::json;
using namespace std::chrono;

namespace hft {
    // Configuration class
    class Config {
    public:
        std::string ws_endpoint;
        std::string symbol;
        double position_size;
        double spread_threshold;
        double max_position;
        std::string api_key;
        std::string api_secret;
        std::string log_path;

        Config(const std::string& config_file) {
            boost::property_tree::ptree pt;
            boost::property_tree::read_ini(config_file, pt);
            
            ws_endpoint = pt.get<std::string>("exchange.ws_endpoint");
            symbol = pt.get<std::string>("trading.symbol");
            position_size = pt.get<double>("trading.position_size");
            spread_threshold = pt.get<double>("trading.spread_threshold");
            max_position = pt.get<double>("trading.max_position");
            api_key = pt.get<std::string>("exchange.api_key");
            api_secret = pt.get<std::string>("exchange.api_secret");
            log_path = pt.get<std::string>("logging.path");
        }
    };

    // Order book structure
    struct OrderBook {
        std::vector<std::pair<double, double>> bids;
        std::vector<std::pair<double, double>> asks;
        int64_t timestamp;
    };

    // Order structure
    struct Order {
        std::string id;
        double price;
        double quantity;
        bool is_buy;
        system_clock::time_point timestamp;
    };

    class HFTSystem {
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
        
        std::atomic<bool> running_{true};
        std::atomic<double> current_position_{0.0};
        std::atomic<int64_t> last_trade_time_{0};
        
        // Performance metrics
        struct Metrics {
            std::atomic<int64_t> trades_executed{0};
            std::atomic<double> latency_sum{0.0};
            std::atomic<int64_t> latency_count{0};
        } metrics_;

    public:
        HFTSystem(const std::string& config_file) : config_(config_file) {
            // Initialize logging
            auto rotating_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                config_.log_path, 1024 * 1024 * 10, 3);
            logger_ = std::make_shared<spdlog::logger>("hft", rotating_sink);
            logger_->set_level(spdlog::level::info);
            spdlog::set_default_logger(logger_);

            // Initialize WebSocket
            ws_client_.clear_access_channels(websocketpp::log::alevel::all);
            ws_client_.init_asio(&io_context_);
            
            setup_handlers();
        }

        void setup_handlers() {
            ws_client_.set_open_handler([this](auto hdl) {
                ws_handle_ = hdl;
                logger_->info("WebSocket connected");
            });
            
            ws_client_.set_fail_handler([this](auto hdl) {
                logger_->error("WebSocket connection failed");
                running_ = false;
            });
            
            ws_client_.set_message_handler([this](auto hdl, auto msg) {
                process_market_data(msg->get_payload());
            });
        }

        void process_market_data(const std::string& payload) {
            try {
                auto start_time = high_resolution_clock::now();
                json data = json::parse(payload);
                
                {
                    std::lock_guard<std::mutex> lock(order_book_mutex_);
                    order_book_.bids.clear();
                    order_book_.asks.clear();
                    
                    for (const auto& bid : data["bids"]) {
                        order_book_.bids.emplace_back(std::stod(bid[0].get<std::string>()),
                                                    std::stod(bid[1].get<std::string>()));
                    }
                    for (const auto& ask : data["asks"]) {
                        order_book_.asks.emplace_back(std::stod(ask[0].get<std::string>()),
                                                    std::stod(ask[1].get<std::string>()));
                    }
                    order_book_.timestamp = data["E"].get<int64_t>();
                }
                
                evaluate_trading_opportunity();
                
                auto end_time = high_resolution_clock::now();
                double latency = duration_cast<microseconds>(end_time - start_time).count();
                metrics_.latency_sum += latency;
                metrics_.latency_count++;
                
            } catch (const std::exception& e) {
                logger_->error("Market data processing error: {}", e.what());
            }
        }

        void evaluate_trading_opportunity() {
            std::lock_guard<std::mutex> lock(order_book_mutex_);
            if (order_book_.bids.empty() || order_book_.asks.empty()) return;

            double best_bid = order_book_.bids[0].first;
            double best_ask = order_book_.asks[0].first;
            double spread = (best_ask - best_bid) / best_bid;

            if (spread > config_.spread_threshold) {
                execute_market_making(best_bid, best_ask);
            }
        }

        void execute_market_making(double bid, double ask) {
            std::lock_guard<std::mutex> lock(position_mutex_);
            
            if (std::abs(current_position_ + config_.position_size) > config_.max_position) {
                logger_->warn("Position limit would be exceeded");
                return;
            }

            try {
                Order buy_order{
                    .id = generate_order_id(),
                    .price = bid * (1 - 0.0001),
                    .quantity = config_.position_size,
                    .is_buy = true,
                    .timestamp = system_clock::now()
                };
                
                Order sell_order{
                    .id = generate_order_id(),
                    .price = ask * (1 + 0.0001),
                    .quantity = config_.position_size,
                    .is_buy = false,
                    .timestamp = system_clock::now()
                };

                // In production: Replace with actual exchange API calls
                simulate_order_execution(buy_order);
                simulate_order_execution(sell_order);
                
                current_position_ += config_.position_size;  // Buy
                current_position_ -= config_.position_size;  // Sell
                metrics_.trades_executed += 2;
                last_trade_time_ = order_book_.timestamp;
                
                logger_->info("Executed trade pair - Buy: {}, Sell: {}", 
                            buy_order.price, sell_order.price);
            } catch (const std::exception& e) {
                logger_->error("Trade execution failed: {}", e.what());
            }
        }

        void simulate_order_execution(const Order& order) {
            // Simulate exchange latency (replace with real API call in production)
            std::this_thread::sleep_for(microseconds(100));
        }

        std::string generate_order_id() {
            static std::atomic<uint64_t> counter{0};
            return "ORD_" + std::to_string(high_resolution_clock::now().time_since_epoch().count()) + 
                  "_" + std::to_string(counter++);
        }

        void monitor_system() {
            while (running_) {
                double avg_latency = metrics_.latency_count > 0 ? 
                    metrics_.latency_sum / metrics_.latency_count : 0;
                
                if (std::abs(current_position_) > config_.max_position * 1.2) {
                    logger_->warn("Position limit exceeded: {}", current_position_.load());
                    // Add emergency unwind logic here
                }
                
                logger_->info("System stats - Trades: {}, Avg Latency: {}μs, Position: {}", 
                            metrics_.trades_executed.load(), avg_latency, current_position_.load());
                
                std::this_thread::sleep_for(seconds(5));
            }
        }

        void run() {
            try {
                websocketpp::lib::error_code ec;
                auto con = ws_client_.get_connection(config_.ws_endpoint, ec);
                if (ec) {
                    logger_->error("Connection failed: {}", ec.message());
                    return;
                }

                ws_client_.connect(con);
                
                std::thread ws_thread([this]() { io_context_.run(); });
                std::thread monitor_thread(&HFTSystem::monitor_system, this);
                
                ws_thread.join();
                monitor_thread.join();
            } catch (const std::exception& e) {
                logger_->error("System error: {}", e.what());
                stop();
            }
        }

        void stop() {
            running_ = false;
            ws_client_.stop();
            io_context_.stop();
        }
    };
}

int main() {
    try {
        hft::HFTSystem system("config.ini");
        system.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
