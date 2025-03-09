#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <string>

namespace hft {
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
}

#endif // CONFIG_HPP
