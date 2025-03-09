#include "../include/hft_system.hpp"
#include "../include/config.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <thread>

namespace hft {
    HFTSystem::HFTSystem(const std::string& config_file) 
        : config_(config_file), running_(true), current_position_(0.0), last_trade_time_(0) {
        auto rotating_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            config_.log_path, 1024 * 1024 * 10, 3);
        logger_ = std::make_shared<spdlog::logger>("hft", rotating_sink);
        logger_->set_level(spdlog::level::info);
        spdlog::set_default_logger(logger_);

        ws_client_.clear_access_channels(websocketpp::log::alevel::all);
        ws_client_.init_asio(&io_context_);
        setup_handlers();
    }

    void HFTSystem::setup_handlers() {
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

    void HFTSystem::process_market_data(const std::string& payload) {
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

    void HFTSystem::evaluate_trading_opportunity() {
        std::lock_guard<std::mutex> lock(order_book_mutex_);
        if (order_book_.bids.empty() || order_book_.asks.empty()) return;

        double best_bid = order_book_.bids[0].first;
        double best_ask = order_book_.asks[0].first;
        double spread = (best_ask - best_bid) / best_bid;

        if (spread > config_.spread_threshold) {
            execute_market_making(best_bid, best_ask);
        }
    }

    void HFTSystem::execute_market_making(double bid, double ask) {
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

            simulate_order_execution(buy_order);
            simulate_order_execution(sell_order);

            current_position_ += config_.position_size;
            current_position_ -= config_.position_size;
            metrics_.trades_executed += 2;
            last_trade_time_ = order_book_.timestamp;

            logger_->info("Executed trade pair - Buy: {}, Sell: {}", 
                         buy_order.price, sell_order.price);
        } catch (const std::exception& e) {
            logger_->error("Trade execution failed: {}", e.what());
        }
    }

    void HFTSystem::simulate_order_execution(const Order& order) {
        std::this_thread::sleep_for(microseconds(100));
    }

    std::string HFTSystem::generate_order_id() {
        static std::atomic<uint64_t> counter{0};
        return "ORD_" + std::to_string(high_resolution_clock::now().time_since_epoch().count()) + 
               "_" + std::to_string(counter++);
    }

    void HFTSystem::monitor_system() {
        while (running_) {
            double avg_latency = metrics_.latency_count > 0 ? 
                metrics_.latency_sum / metrics_.latency_count : 0;

            if (std::abs(current_position_) > config_.max_position * 1.2) {
                logger_->warn("Position limit exceeded: {}", current_position_.load());
            }

            logger_->info("System stats - Trades: {}, Avg Latency: {}μs, Position: {}", 
                         metrics_.trades_executed.load(), avg_latency, current_position_.load());

            std::this_thread::sleep_for(seconds(5));
        }
    }

    void HFTSystem::run() {
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

    void HFTSystem::stop() {
        running_ = false;
        ws_client_.stop();
        io_context_.stop();
    }
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
