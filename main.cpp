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
    
    int64_t lastSeqId = -1;
    mutable std::mutex mtx;

public:
    // 【核心改动】处理 OKX 增量，并触发本地撮合检查
    void updateLevel(Side side, double price, double totalQuantity) {
        std::lock_guard<std::mutex> lock(mtx);
        if (side == BUY) {
            updateMap(bids, price, totalQuantity, BUY);
        } else {
            updateMap(asks, price, totalQuantity, SELL);
        }
        // 关键：每当行情变动，检查是否有本地订单可以成交
        checkAndMatchLocalOrders();
    }

    void limitOrder(uint64_t id, double price, double quantity, Side side, std::string symbol) {
        std::lock_guard<std::mutex> lock(mtx);
        if (order_lookup.count(id)) return;
        
        Order new_order(id, price, quantity, side, symbol);
        
        // 下单时先尝试立即成交 (Taker 行为)
        if (side == BUY) match(new_order, asks);
        else match(new_order, bids);

        // 如果没成交完，挂在账本上 (Maker 行为)
        if (new_order.quantity > 1e-10) {
            if (side == BUY) {
                auto& queue = bids[price];
                queue.push_back(new_order);
                order_lookup[id] = (OrderEntry){price, side, --queue.end()};
            } else {
                auto& queue = asks[price];
                queue.push_back(new_order);
                order_lookup[id] = (OrderEntry){price, side, --queue.end()};
            }
            std::cout << "\n[本地] 订单 " << id << " 已挂单: " << (side==BUY?"买入":"卖出") << " @ " << price << std::endl;
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mtx);
        asks.clear(); bids.clear(); order_lookup.clear(); lastSeqId = -1;
    }

    void setSeqId(int64_t seqId) { lastSeqId = seqId; }
    int64_t getSeqId() const { return lastSeqId; }

    void displayDepth(int levels = 5) const {
        std::lock_guard<std::mutex> lock(mtx);
        if (asks.empty() && bids.empty()) return;

        std::cout << "\n========== OKX 实时深度 (seqId: " << lastSeqId << ") ==========" << std::endl;
        int count = 0;
        std::vector<std::pair<double, double> > top_asks;
        for (auto it = asks.begin(); it != asks.end() && count < levels; ++it, ++count) {
            double level_qty = 0;
            for (auto const& o : it->second) level_qty += o.quantity;
            top_asks.push_back(std::make_pair(it->first, level_qty));
        }
        std::reverse(top_asks.begin(), top_asks.end());
        for (auto const& p : top_asks) {
            std::cout << std::setw(12) << std::fixed << std::setprecision(1) << p.first << " | " << std::setprecision(4) << p.second << " (卖)" << std::endl;
        }
        std::cout << "--------------------------------------------------" << std::endl;
        count = 0;
        for (auto it = bids.begin(); it != bids.end() && count < levels; ++it, ++count) {
            double level_qty = 0;
            for (auto const& o : it->second) level_qty += o.quantity;
            std::cout << std::setw(12) << std::fixed << std::setprecision(1) << it->first << " | " << std::setprecision(4) << level_qty << " (买)" << std::endl;
        }
        std::cout << "==================================================" << std::endl;
        std::cout << std::defaultfloat;
    }

private:
    // 【核心逻辑】检查本地单是否被行情“撞击”
    void checkAndMatchLocalOrders() {
        if (bids.empty() || asks.empty()) return;

        // 如果买一 >= 卖一，说明有成交空间
        auto b_it = bids.begin();
        auto a_it = asks.begin();

        while (b_it != bids.end() && a_it != asks.end() && b_it->first >= a_it->first) {
            auto& b_queue = b_it->second;
            auto& a_queue = a_it->second;

            auto b_q_it = b_queue.begin();
            while (b_q_it != b_queue.end() && !a_queue.empty()) {
                auto a_q_it = a_queue.begin();
                while (a_q_it != a_queue.end() && b_q_it->quantity > 1e-10) {
                    // 只有当其中一个是本地单 (id != 0) 时，我们才感兴趣
                    if (b_q_it->id != 0 || a_q_it->id != 0) {
                        double fill_qty = std::min(b_q_it->quantity, a_q_it->quantity);
                        std::cout << "\n\a<<<<< 虚拟成交触发! >>>>>" << std::endl;
                        std::cout << "订单 " << (b_q_it->id ? std::to_string(b_q_it->id) : "OKX") << " (买) 与 "
                                  << (a_q_it->id ? std::to_string(a_q_it->id) : "OKX") << " (卖) 成交" << std::endl;
                        std::cout << std::fixed << std::setprecision(1) << "价格: " << a_it->first 
                                  << " | 数量: " << std::setprecision(4) << fill_qty << std::endl;
                        std::cout << "<<<<<<<<<<<<<<<<<<<<<<<<<\n" << std::endl;
                        std::cout << std::defaultfloat; // 恢复默认格式
                        
                        b_q_it->quantity -= fill_qty;
                        a_q_it->quantity -= fill_qty;
                    } else {
                        // 如果两个都是 OKX 的行情单，我们不干涉，直接跳过
                        // (在真实系统中，这代表交易所内部发生的成交，我们只需更新本地深度即可)
                        break; 
                    }

                    if (a_q_it->quantity < 1e-10) {
                        if (a_q_it->id != 0) order_lookup.erase(a_q_it->id);
                        a_q_it = a_queue.erase(a_q_it);
                    } else { ++a_q_it; }
                }
                if (b_q_it->quantity < 1e-10) {
                    if (b_q_it->id != 0) order_lookup.erase(b_q_it->id);
                    b_q_it = b_queue.erase(b_q_it);
                } else { ++b_q_it; }
            }

            if (b_queue.empty()) bids.erase(b_it++);
            else if (a_queue.empty()) asks.erase(a_it++);
            else break;
        }
    }

    template<typename M>
    void updateMap(M& side_map, double price, double totalQuantity, Side side) {
        if (totalQuantity < 1e-10) {
            side_map.erase(price);
        } else {
            auto& queue = side_map[price];
            // OKX 的单子 ID 统一设为 0
            if (queue.empty() || queue.front().id == 0) {
                if (queue.empty()) queue.push_back(Order(0, price, totalQuantity, side, "OKX"));
                else queue.front().quantity = totalQuantity;
            } else {
                // 如果该价位有本地单，行情单排在本地单后面（模拟时间优先）
                // 实际同步中这种情况较少，因为 L2 是聚合的
                queue.front().quantity = totalQuantity; 
            }
        }
    }

    template<typename T>
    void match(Order& incoming, T& counter_side_map) {
        auto it = counter_side_map.begin();
        while (it != counter_side_map.end() && incoming.quantity > 1e-10) {
            if ((incoming.side == BUY && incoming.price < it->first) || 
                (incoming.side == SELL && incoming.price > it->first)) break;

            auto& queue = it->second;
            auto q_it = queue.begin();
            while (q_it != queue.end() && incoming.quantity > 1e-10) {
                double fill_qty = std::min(incoming.quantity, q_it->quantity);
                std::cout << "\n[成交] 订单 " << incoming.id << " 成交价格: " 
                          << std::fixed << std::setprecision(1) << it->first 
                          << " 数量: " << std::setprecision(4) << fill_qty << std::endl;
                std::cout << std::defaultfloat; // 恢复默认格式
                incoming.quantity -= fill_qty;
                q_it->quantity -= fill_qty;
                if (q_it->quantity < 1e-10) {
                    if (q_it->id != 0) order_lookup.erase(q_it->id);
                    q_it = queue.erase(q_it);
                } else { ++q_it; }
            }
            if (queue.empty()) counter_side_map.erase(it++);
            else ++it;
        }
    }
};

void startOkxSync(OrderBook& ob, const std::string& instId) {
    static ix::WebSocket webSocket;
    webSocket.setUrl("wss://ws.okx.com:8443/ws/v5/public");
    ix::SocketTLSOptions tlsOptions; tlsOptions.caFile = "NONE"; webSocket.setTLSOptions(tlsOptions);

    webSocket.setOnMessageCallback([&ob, instId](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message) {
            try {
                auto j = json::parse(msg->str);
                if (j.contains("arg") && j["arg"]["channel"] == "books") {
                    if (j.value("action", "") == "snapshot") ob.clear();
                    if (j.contains("data")) {
                        for (auto& item : j["data"]) {
                            ob.setSeqId(item.value("seqId", (int64_t)-1));
                            for (auto& b : item["bids"]) ob.updateLevel(BUY, std::stod(b[0].get<std::string>()), std::stod(b[1].get<std::string>()));
                            for (auto& a : item["asks"]) ob.updateLevel(SELL, std::stod(a[0].get<std::string>()), std::stod(a[1].get<std::string>()));
                        }
                    }
                }
            } catch (...) {}
        } else if (msg->type == ix::WebSocketMessageType::Open) {
            json sub = {{"op", "subscribe"}, {"args", {{{"channel", "books"}, {"instId", instId}}}}};
            webSocket.send(sub.dump());
        }
    });
    webSocket.start();
}

int main() {
    OrderBook ob;
    startOkxSync(ob, "BTC-USDT");

    // 模拟策略：在 5 秒后，根据当前价格“埋伏”一个虚拟订单
    // std::this_thread::sleep_for(std::chrono::seconds(5));
    
    // std::cout << "\n[策略] 正在埋伏一个虚拟买单..." << std::endl;
    // ob.limitOrder(8888, 1000000.0, 20, BUY, "BTC-USDT"); // 请根据当前实际 BTC 价格修改 95000.0

    while (true) {
        ob.displayDepth(5);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    return 0;
}
