#include <iostream>
#include <map>
#include <unordered_map>
#include <list>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <stdexcept>
#include <mutex>
#include <thread>

// 引入依赖库
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// 订单方向
enum Side { BUY, SELL };

struct Order {
    uint64_t id; 
    double price;
    double quantity;         
    double originalQuantity; 
    double filledQuantity;   
    Side side; 
    std::string symbol; 
    long long timestamp;

    Order(uint64_t _id, double _price, double _qty, Side _side, std::string _symbol)
        : id(_id), price(_price), quantity(_qty), originalQuantity(_qty), filledQuantity(0), side(_side), symbol(_symbol) {
        timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    }
};

struct OrderEntry {
    double price;
    Side side;
    std::list<Order>::iterator it;
};

class OrderBook {
private:
    std::map<double, std::list<Order> > asks;
    std::map<double, std::list<Order>, std::greater<double> > bids;
    std::unordered_map<uint64_t, OrderEntry> order_lookup;
    
    int64_t lastSeqId = -1; // 用于追踪序列号
    mutable std::mutex mtx;

public:
    void updateLevel(Side side, double price, double totalQuantity) {
        std::lock_guard<std::mutex> lock(mtx);
        if (side == BUY) {
            updateMap(bids, price, totalQuantity, BUY);
        } else {
            updateMap(asks, price, totalQuantity, SELL);
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mtx);
        asks.clear();
        bids.clear();
        order_lookup.clear();
        lastSeqId = -1;
    }

    void setSeqId(int64_t seqId) { lastSeqId = seqId; }
    int64_t getSeqId() const { return lastSeqId; }

    void displayDepth(int levels = 5) const {
        std::lock_guard<std::mutex> lock(mtx);
        if (asks.empty() && bids.empty()) {
            std::cout << "\r[等待数据] 正在同步 OKX 400档账本... seqId: " << lastSeqId << std::flush;
            return;
        }

        std::cout << "\n========== OKX 400档深度 (seqId: " << lastSeqId << ") ==========" << std::endl;
        std::cout << std::setw(12) << "价格 (Price)" << " | " << "数量 (Qty)" << std::endl;
        
        int count = 0;
        std::vector<std::pair<double, double> > top_asks;
        for (auto it = asks.begin(); it != asks.end() && count < levels; ++it, ++count) {
            double level_qty = 0;
            for (auto const& o : it->second) level_qty += o.quantity;
            top_asks.push_back(std::make_pair(it->first, level_qty));
        }
        std::reverse(top_asks.begin(), top_asks.end());
        for (auto const& p : top_asks) {
            // 使用 fixed 确保价格不进入科学计数法，setprecision(1) 保留一位小数
            std::cout << std::setw(12) << std::fixed << std::setprecision(1) << p.first 
                      << " | " << std::setprecision(4) << p.second << " (卖)" << std::endl;
        }

        std::cout << "--------------------------------------------------" << std::endl;

        count = 0;
        for (auto it = bids.begin(); it != bids.end() && count < levels; ++it, ++count) {
            double level_qty = 0;
            for (auto const& o : it->second) level_qty += o.quantity;
            std::cout << std::setw(12) << std::fixed << std::setprecision(1) << it->first 
                      << " | " << std::setprecision(4) << level_qty << " (买)" << std::endl;
        }
        std::cout << "==================================================\n" << std::endl;
        std::cout << std::defaultfloat;
    }

private:
    template<typename M>
    void updateMap(M& side_map, double price, double totalQuantity, Side side) {
        if (totalQuantity < 1e-10) {
            side_map.erase(price);
        } else {
            std::list<Order>& queue = side_map[price];
            if (queue.empty()) {
                queue.push_back(Order(0, price, totalQuantity, side, "OKX_SYNC"));
            } else {
                queue.front().quantity = totalQuantity;
            }
        }
    }
};

void startOkxSync(OrderBook& ob, const std::string& instId) {
    static ix::WebSocket webSocket;
    // 使用文档要求的 8443 端口
    std::string url = "wss://ws.okx.com:8443/ws/v5/public";
    webSocket.setUrl(url);

    ix::SocketTLSOptions tlsOptions;
    tlsOptions.caFile = "NONE"; 
    webSocket.setTLSOptions(tlsOptions);

    webSocket.setOnMessageCallback([&ob, instId](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message) {
            try {
                auto j = json::parse(msg->str);
                
                if (j.contains("event")) {
                    if (j["event"] == "subscribe") {
                        std::cout << "\n[系统] 成功订阅 " << instId << " 400档深度频道" << std::endl;
                    }
                    return;
                }

                if (j.contains("arg") && j["arg"]["channel"] == "books") {
                    std::string action = j.value("action", "");
                    
                    if (j.contains("data")) {
                        for (auto& item : j["data"]) {
                            int64_t seqId = item.value("seqId", (int64_t)-1);
                            int64_t prevSeqId = item.value("prevSeqId", (int64_t)-1);

                            // 序列号校验
                            if (action == "snapshot") {
                                ob.clear();
                            } else if (action == "update") {
                                if (prevSeqId != -1 && ob.getSeqId() != -1 && prevSeqId != ob.getSeqId()) {
                                    // 序列重置异常情况在文档中有描述，此处简单记录
                                    if (seqId < prevSeqId) {
                                        std::cout << "\n[系统] 序列号重置 (Maintenance)" << std::endl;
                                    } else if (prevSeqId != ob.getSeqId()) {
                                        std::cerr << "\n[系统] 丢包警告! 本地 seqId: " << ob.getSeqId() << " != 收到 prevSeqId: " << prevSeqId << std::endl;
                                    }
                                }
                            }
                            ob.setSeqId(seqId);

                            // 更新买单
                            if (item.contains("bids")) {
                                for (auto& b : item["bids"]) {
                                    ob.updateLevel(BUY, std::stod(b[0].get<std::string>()), std::stod(b[1].get<std::string>()));
                                }
                            }
                            // 更新卖单
                            if (item.contains("asks")) {
                                for (auto& a : item["asks"]) {
                                    ob.updateLevel(SELL, std::stod(a[0].get<std::string>()), std::stod(a[1].get<std::string>()));
                                }
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                // 忽略异常数据
            }
        } else if (msg->type == ix::WebSocketMessageType::Open) {
            std::cout << "[网络] 连接已开启，正在请求快照..." << std::endl;
            json sub = {
                {"op", "subscribe"},
                {"args", {{
                    {"channel", "books"},
                    {"instId", instId}
                }}}
            };
            webSocket.send(sub.dump());
        }
    });

    webSocket.start();
}

int main() {
    OrderBook ob;
    std::string symbol = "BTC-USDT";

    startOkxSync(ob, symbol);

    while (true) {
        ob.displayDepth(10);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    return 0;
}
