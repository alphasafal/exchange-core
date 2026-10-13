#include "model/compare.hpp"

#include <sstream>

#include "xc/core/depth.hpp"

namespace xc::model {

std::string diff_fills(const std::vector<Fill>& engine, const std::vector<Fill>& model) {
    if (engine.size() != model.size()) {
        std::ostringstream out;
        out << "fill count " << engine.size() << " != " << model.size();
        return out.str();
    }
    for (std::size_t i = 0; i < engine.size(); ++i) {
        const Fill& a = engine[i];
        const Fill& e = model[i];
        if (a.price != e.price || a.quantity != e.quantity ||
            a.aggressor_order != e.aggressor_order || a.resting_order != e.resting_order ||
            a.aggressor_side != e.aggressor_side || a.resting_filled != e.resting_filled) {
            std::ostringstream out;
            out << "fill " << i << " differs: engine {px=" << a.price << " qty=" << a.quantity
                << " agg=" << a.aggressor_order.value() << " rest=" << a.resting_order.value()
                << "} vs model {px=" << e.price << " qty=" << e.quantity
                << " agg=" << e.aggressor_order.value() << " rest=" << e.resting_order.value()
                << "}";
            return out.str();
        }
    }
    return {};
}

std::string diff_books(const OrderBook& engine, const ReferenceBook& model) {
    std::vector<Order> engine_orders;
    engine.for_each_resting_order([&](const Order& order) { engine_orders.push_back(order); });
    const std::vector<Order> model_orders = model.resting_orders();

    if (engine_orders.size() != model_orders.size()) {
        std::ostringstream out;
        out << "resting order count " << engine_orders.size() << " != " << model_orders.size();
        return out.str();
    }
    for (std::size_t i = 0; i < engine_orders.size(); ++i) {
        const Order& a = engine_orders[i];
        const Order& e = model_orders[i];
        if (a.id != e.id || a.price != e.price || a.remaining != e.remaining ||
            a.quantity != e.quantity || a.side != e.side || a.sequence != e.sequence) {
            std::ostringstream out;
            out << "resting order " << i << " differs: engine {id=" << a.id.value()
                << " px=" << a.price << " rem=" << a.remaining << " qty=" << a.quantity
                << " seq=" << a.sequence << "} vs model {id=" << e.id.value() << " px=" << e.price
                << " rem=" << e.remaining << " qty=" << e.quantity << " seq=" << e.sequence << "}";
            return out.str();
        }
    }

    const TopOfBook engine_top = engine.top_of_book();
    const TopOfBook model_top = model.top_of_book();
    if (engine_top.bid_price != model_top.bid_price ||
        engine_top.ask_price != model_top.ask_price ||
        engine_top.bid_quantity != model_top.bid_quantity ||
        engine_top.ask_quantity != model_top.ask_quantity) {
        std::ostringstream out;
        out << "top of book differs: engine {" << engine_top.bid_price << "x"
            << engine_top.bid_quantity << " / " << engine_top.ask_price << "x"
            << engine_top.ask_quantity << "} vs model {" << model_top.bid_price << "x"
            << model_top.bid_quantity << " / " << model_top.ask_price << "x"
            << model_top.ask_quantity << "}";
        return out.str();
    }

    DepthSnapshot engine_depth;
    DepthSnapshot model_depth;
    engine.depth(10, engine_depth);
    model.depth(10, model_depth);
    if (engine_depth.bids != model_depth.bids || engine_depth.asks != model_depth.asks) {
        return "aggregated depth differs";
    }

    return {};
}

}  // namespace xc::model
