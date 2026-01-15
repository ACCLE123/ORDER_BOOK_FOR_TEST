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

// 订单方向
enum Side { BUY, SELL };

// 订单结构体
struct Order {
    uint64_t id; // 订单ID（唯一的 相当于主键）
    double price;
    uint32_t quantity;         // 当前剩余数量
    uint32_t originalQuantity; // 订单原始数量
    uint32_t filledQuantity;   // 订单已成交数量
    Side side; // 交易方向
    std::string symbol; // 标的
    long long timestamp;

    Order(uint64_t _id, double _price, uint32_t _qty, Side _side, std::string _symbol)
        : id(_id), price(_price), quantity(_qty), originalQuantity(_qty), filledQuantity(0), side(_side), symbol(_symbol) {
        timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    }
};

// 订单簿存储索引
struct OrderEntry {
    double price;
    Side side;
    std::list<Order>::iterator it;
    // 通过price side 找到 对应的 list
    // 通过it 找到对应的订单
};

class OrderBook {
private:
    // 越靠前的越容易被交易

    // 卖单：价格低到高
    std::map<double, std::list<Order> > asks;
    // 买单：价格高到低
    std::map<double, std::list<Order>, std::greater<double> > bids;

    std::unordered_map<uint64_t, OrderEntry> order_lookup;

public:
    
    void limitOrder(uint64_t id, double price, uint32_t quantity, Side side, std::string symbol) {
        if (order_lookup.count(id)) {
            throw std::invalid_argument("Order ID already exists.");
        }
        if (quantity == 0) return;

        Order new_order(id, price, quantity, side, symbol);

        // 看看能不能直接买到 或者 直接卖出
        if (side == BUY) {
            match(new_order, asks);
        } else {
            match(new_order, bids);
        }

        // 假如订单还在 则需要加入订单簿
        if (new_order.quantity > 0) {
            if (side == BUY) {
                std::list<Order>& queue = bids[price];
                queue.push_back(new_order);
                order_lookup[id] = (OrderEntry){price, side, --queue.end()};
            } else {
                std::list<Order>& queue = asks[price];
                queue.push_back(new_order);
                order_lookup[id] = (OrderEntry){price, side, --queue.end()};
            }
        }
    }

    void cancelOrder(uint64_t id) {
        auto it = order_lookup.find(id);
        if (it == order_lookup.end()) {
            std::cout << "Warning: Order ID " << id << " not found." << std::endl;
            return;
        }

        const OrderEntry& entry = it->second;
        if (entry.side == BUY) {
            bids[entry.price].erase(entry.it);
            if (bids[entry.price].empty()) bids.erase(entry.price);
        } else {
            asks[entry.price].erase(entry.it);
            if (asks[entry.price].empty()) asks.erase(entry.price);
        }

        order_lookup.erase(it);
        std::cout << "Order " << id << " cancelled." << std::endl;
    }

    void displayDepth() const {
        std::cout << "\n========== 深度 ==========" << std::endl;
        std::cout << std::setw(10) << "卖单" << std::endl;
        std::cout << std::setw(10) << "Price" << " | " << "Quantity" << std::endl;
        
        int count = 0;
        std::vector<std::pair<double, uint32_t> > top_asks; // 卖单存储是从低到高
        // 打印的时候 卖单在上面 从高到低  
        for (auto it = asks.begin(); it != asks.end() && count < 5; ++it, ++count) {
            uint32_t level_qty = 0;
            for (auto lit = it->second.begin(); lit != it->second.end(); ++lit) level_qty += lit->quantity;
            top_asks.push_back(std::make_pair(it->first, level_qty));
        }
        std::reverse(top_asks.begin(), top_asks.end());
        for (size_t i = 0; i < top_asks.size(); ++i) {
            std::cout << std::setw(10) << top_asks[i].first << " | " << top_asks[i].second << std::endl;
        }

        std::cout << "----------------------------------" << std::endl;

        count = 0;
        for (auto it = bids.begin(); it != bids.end() && count < 5; ++it, ++count) {
            uint32_t level_qty = 0;
            for (auto lit = it->second.begin(); lit != it->second.end(); ++lit) level_qty += lit->quantity;
            std::cout << std::setw(10) << it->first << " | " << level_qty << std::endl;
        }
        std::cout << std::setw(10) << "买单" << std::endl;
        std::cout << "==================================\n" << std::endl;
    }

private:
    // 撮合函数
    //  这里用泛型传入 bids 和 asks T 相当于map<double, list<Order>>
    // incoming 新进入的订单 counter_side_map对手订单(map<double, list<Order>>)
    template<typename T>
    void match(Order& incoming, T& counter_side_map) {
        // 越靠前的越先交易

        // 先看价格
        // 以订单为买单 对手订单为卖单（从小到大） 为例 
        // 先看卖得最便宜的

        // 这里需要遍历 来实现大买单吃掉多个小单的情况，不可以只拿begin()
        auto it = counter_side_map.begin();
        while (it != counter_side_map.end() && incoming.quantity > 0) { 
            double best_price = it->first;
            
            bool can_match = (incoming.side == BUY) ? (incoming.price >= best_price) : (incoming.price <= best_price);
            // 买的话 价格要>= 卖得最便宜的 
            // 卖的话 价格要<= 买得最贵的

            // 没匹配上
            if (!can_match) break;
        
            
            std::list<Order>& queue = it->second;
            
            // 同理需要遍历，这里的遍历是按照时间顺序的
            auto q_it = queue.begin();
            while (q_it != queue.end() && incoming.quantity > 0) {
                Order& maker = *q_it; // 成交的对手订单

                uint32_t fill_qty = std::min(incoming.quantity, maker.quantity); // 成交量

                std::cout << "MATCH: Order " << incoming.id << " (" << (incoming.side == BUY ? "BUY" : "SELL") 
                          << ") matched with Order " << maker.id << " (" << (maker.side == BUY ? "BUY" : "SELL")
                          << ") | Qty: " << fill_qty << " @ Price: " << maker.price << std::endl;

                incoming.quantity -= fill_qty;
                incoming.filledQuantity += fill_qty;
                maker.quantity -= fill_qty;
                maker.filledQuantity += fill_qty;

                if (maker.quantity == 0) {
                    order_lookup.erase(maker.id);
                    q_it = queue.erase(q_it);
                } else {
                    ++q_it;
                }
            }

            if (queue.empty()) {
                counter_side_map.erase(it++);
            } else {
                ++it;
            }
        }
    }
};

int main() {
    OrderBook ob;

    try {
        // 删除订单 
        ob.limitOrder(5, 12, 10, SELL, "TEST");
        ob.displayDepth();

        ob.cancelOrder(5);
        ob.displayDepth();


        // 打印深度为5的订单簿

        // for (int i = 0; i < 7; ++i) {
        //     ob.limitOrder(100 + i, 8.0 - i * 0.1, 10, BUY, "TEST");
        //     ob.limitOrder(200 + i, 15.0 + i * 0.1, 10, SELL, "TEST");
        // }
        // ob.displayDepth();


        
        // 撮合订单
        // ob.limitOrder(1, 10, 100, BUY, "TEST");
        // ob.limitOrder(2, 11, 50, BUY, "TEST");

        // ob.limitOrder(3, 9, 100, SELL, "TEST");

        // ob.displayDepth();

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    return 0;
}
