// SPDX-License-Identifier: MIT
#include "hub.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#endif

namespace hids {
namespace {

int32_t narrow(const int64_t value) noexcept {
    return static_cast<int32_t>(std::clamp<int64_t>(value, std::numeric_limits<int32_t>::min(),
                                                    std::numeric_limits<int32_t>::max()));
}

uint32_t micros(const int64_t from, const int64_t to) noexcept {
    const int64_t span = (to - from) / 1000;
    return static_cast<uint32_t>(
        std::clamp<int64_t>(span, 0, std::numeric_limits<uint32_t>::max()));
}

void note_worst(std::atomic<uint32_t>& worst, const uint32_t value) noexcept {
    uint32_t seen = worst.load(std::memory_order_relaxed);
    while (value > seen && !worst.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}

uint32_t percentile(const std::vector<uint32_t>& sorted, const unsigned percent) noexcept {
    if (sorted.empty())
        return 0;
    const size_t index = (sorted.size() * percent + 99) / 100;
    return sorted[std::min(index, sorted.size()) - 1];
}

// The sender sleeps on timers and condition variables: a timer slack of one
// nanosecond keeps its wake-ups from being rounded up, and a real-time
// priority, where the user is allowed one, keeps another process from
// running in front of it. True when the priority was granted.
bool favour_this_thread() noexcept {
#ifdef __linux__
    static_cast<void>(prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL));
    sched_param parameter{};
    parameter.sched_priority = 20;
    return pthread_setschedparam(pthread_self(), SCHED_RR, &parameter) == 0;
#else
    return false;
#endif
}

} // namespace

int64_t Hub::now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}


Hub::Hub() {
    for (int link = 0; link < max_links; ++link)
        links_[static_cast<size_t>(link)].sink.bind(this, link);
    backends_.push_back(make_aoa_backend());
    backends_.push_back(make_mim_backend());
    found_counts_.assign(backends_.size(), 0);
}

Hub::~Hub() { stop(); }

std::string Hub::start() {
    start_errors_.assign(backends_.size(), {});
    std::string errors;
    size_t started = 0;
    for (size_t each = 0; each < backends_.size(); ++each) {
        start_errors_[each] = backends_[each]->start();
        if (start_errors_[each].empty())
            ++started;
        else
            errors += (errors.empty() ? "" : "; ") + start_errors_[each];
    }
    return started == 0 ? errors : std::string();
}

void Hub::stop() {
    for (Link& link : links_) {
        if (!link.sender.joinable())
            continue;
        {
            const std::lock_guard<std::mutex> lock(link.queue);
            link.stopping = true;
        }
        link.wake.notify_one();
        link.sender.join();
    }
    for (Link& link : links_) {
        const std::lock_guard<std::mutex> lock(link.port_lock);
        close_locked(link);
    }
    const std::lock_guard<std::mutex> lock(scan_lock_);
    for (const std::unique_ptr<Backend>& backend : backends_)
        backend->stop();
    found_.clear();
    found_counts_.assign(backends_.size(), 0);
}

std::vector<TargetInfo> Hub::scan(const ScanRequest& request, std::string& error) {
    const std::lock_guard<std::mutex> lock(scan_lock_);
    found_.clear();
    found_counts_.assign(backends_.size(), 0);
    for (size_t each = 0; each < backends_.size(); ++each) {
        std::string note;
        if (each < start_errors_.size() && !start_errors_[each].empty()) {
            error += (error.empty() ? "" : "; ") + std::string(backends_[each]->name()) + ": " +
                     start_errors_[each];
            continue;
        }
        std::vector<TargetInfo> part = backends_[each]->scan(request, note);
        if (!note.empty())
            error += (error.empty() ? "" : "; ") + std::string(backends_[each]->name()) + ": " + note;
        found_counts_[each] = part.size();
        for (TargetInfo& target : part)
            found_.push_back(std::move(target));
    }
    return found_;
}

int Hub::open(const size_t index, const LinkOptions& options, std::string& error) {
    // The entry of the last scan is borrowed for the whole call.
    const std::lock_guard<std::mutex> scanning(scan_lock_);
    Backend* backend = nullptr;
    size_t local = index;
    for (size_t each = 0; each < backends_.size() && backend == nullptr; ++each) {
        if (local < found_counts_[each])
            backend = backends_[each].get();
        else
            local -= found_counts_[each];
    }
    if (backend == nullptr || index >= found_.size()) {
        error = "the target is not in the last scan";
        return -1;
    }
    const std::string serial = found_[index].serial;

    // A target goes back to the slot it had, then to a slot that has had none:
    // a Context hands out HID ids once, and Android tears the old ones down
    // slowly.
    int slot = -1;
    {
        const std::lock_guard<std::mutex> lock(slots_);
        const auto pick = [&](const auto& wanted) {
            for (int link = 0; link < max_links && slot < 0; ++link) {
                const Link& entry = links_[static_cast<size_t>(link)];
                if (!entry.taken && wanted(entry))
                    slot = link;
            }
        };
        pick([&](const Link& entry) { return !serial.empty() && entry.last_serial == serial; });
        pick([](const Link& entry) { return entry.last_serial.empty(); });
        pick([](const Link&) { return true; });
        if (slot >= 0)
            links_[static_cast<size_t>(slot)].taken = true;
    }
    if (slot < 0) {
        error = "too many targets are open";
        return -1;
    }
    Link& link = links_[static_cast<size_t>(slot)];
    const auto give_up = [&](const std::string& why) {
        error = why;
        const std::lock_guard<std::mutex> lock(slots_);
        link.taken = false;
        return -1;
    };

    {
        const std::lock_guard<std::mutex> lock(link.port_lock);
        std::unique_ptr<Port> port = backend->open(local, slot, options, error);
        if (port == nullptr)
            return give_up(error);
        link.port = std::move(port);
        link.node_ready_ns = {};
        link.touch_down = {};
    }

    {
        const std::lock_guard<std::mutex> lock(link.queue);
        link.motion = {};
        link.jumps.clear();
        link.swipe = {};
        link.touch_queued_down = {};
        link.lift_owed = {};
        link.trace_until.store(0, std::memory_order_relaxed);
        link.head = link.count = 0;
        link.stopping = false;
    }
    {
        const std::lock_guard<std::mutex> lock(link.note);
        link.failure.clear();
    }
    link.reports.store(0, std::memory_order_relaxed);
    link.merged.store(0, std::memory_order_relaxed);
    link.ring_next.store(0, std::memory_order_relaxed);
    link.queue_worst.store(0, std::memory_order_relaxed);
    link.total_worst.store(0, std::memory_order_relaxed);
    link.failed.store(false, std::memory_order_relaxed);
    link.last_serial = serial;
    if (!link.sender.joinable())
        link.sender = std::thread([this, &link] { run(link); });
    link.open.store(true, std::memory_order_release);
    return slot;
}

void Hub::set_nodes(const int link, const LinkOptions& options) {
    if (link < 0 || link >= max_links)
        return;
    Link& entry = links_[static_cast<size_t>(link)];
    const std::lock_guard<std::mutex> lock(entry.port_lock);
    if (entry.port == nullptr || !entry.open.load(std::memory_order_acquire))
        return;
    const int64_t ready = now_ns() + entry.port->settle_ns();
    uint8_t added = 0;
    const bool synced = entry.port->set_nodes(options, added);
    for (size_t kind = 0; kind < node_kinds; ++kind) {
        if ((added & node_bit(static_cast<NodeKind>(kind))) != 0)
            entry.node_ready_ns[kind] = ready;
    }
    if ((added & node_bit(node_touch)) != 0)
        entry.touch_down = {};
    if (!synced)
        fail(entry, entry.port->error());
}

void Hub::close_locked(Link& link) {
    link.open.store(false, std::memory_order_release);
    if (link.port != nullptr)
        link.port->close();
    link.port.reset();
    link.touch_down = {};
}

void Hub::close(const int link) {
    if (link < 0 || link >= max_links)
        return;
    Link& entry = links_[static_cast<size_t>(link)];
    {
        const std::lock_guard<std::mutex> lock(entry.port_lock);
        close_locked(entry);
    }
    const std::lock_guard<std::mutex> lock(slots_);
    entry.taken = false;
}

void Hub::check(const int link) {
    if (link < 0 || link >= max_links)
        return;
    Link& entry = links_[static_cast<size_t>(link)];
    std::unique_lock<std::mutex> lock(entry.port_lock, std::try_to_lock);
    if (!lock.owns_lock() || !entry.open.load(std::memory_order_acquire) || entry.port == nullptr)
        return;
    if (!entry.port->healthy())
        fail(entry, entry.port->error());
}

bool Hub::alive(const int link) const noexcept {
    if (link < 0 || link >= max_links)
        return false;
    const Link& entry = links_[static_cast<size_t>(link)];
    return entry.open.load(std::memory_order_acquire) &&
           !entry.failed.load(std::memory_order_acquire);
}

std::string Hub::failure(const int link) const {
    if (link < 0 || link >= max_links)
        return {};
    const Link& entry = links_[static_cast<size_t>(link)];
    const std::lock_guard<std::mutex> lock(entry.note);
    return entry.failure;
}

uint64_t Hub::reports(const int link) const noexcept {
    if (link < 0 || link >= max_links)
        return 0;
    return links_[static_cast<size_t>(link)].reports.load(std::memory_order_relaxed);
}

std::vector<std::string> Hub::trace(const int link) {
    std::vector<std::string> lines;
    if (link < 0 || link >= max_links)
        return lines;
    Link& entry = links_[static_cast<size_t>(link)];
    constexpr int64_t keep_on_ns = 2000000000;
    entry.trace_until.store(now_ns() + keep_on_ns, std::memory_order_relaxed);
    const std::lock_guard<std::mutex> lock(entry.trace_lock);
    const size_t size = entry.trace_ring.size();
    const uint64_t first = entry.trace_next > size ? entry.trace_next - size : 0;
    for (uint64_t number = first; number < entry.trace_next; ++number) {
        const Trace& call = entry.trace_ring[number % size];
        std::string text;
        switch (call.kind) {
        case Trace::mouse_move:
            text = "mouse move dx " + std::to_string(call.a) + " dy " + std::to_string(call.b);
            break;
        case Trace::mouse_scroll:
            text =
                "mouse scroll wheel " + std::to_string(call.a) + " pan " + std::to_string(call.b);
            break;
        case Trace::button:
            text = "mouse button " + std::to_string(call.a) + (call.phase != 0 ? " down" : " up");
            break;
        case Trace::key:
            text = "key usage " + std::to_string(call.a) + (call.phase != 0 ? " down" : " up");
            break;
        case Trace::media:
            text = "media usage " + std::to_string(call.a) + (call.phase != 0 ? " down" : " up");
            break;
        case Trace::touch:
            text = "touch contact " + std::to_string(call.a) +
                   (call.phase == 0   ? " lift"
                    : call.phase == 1 ? " down"
                                      : " move") +
                   " x " + std::to_string(call.b) + " y " + std::to_string(call.c);
            break;
        }
        lines.push_back(std::to_string(number) + " " + std::to_string(call.at / 1000000) + " " +
                        std::to_string(call.report) + " " + text);
    }
    return lines;
}

LinkStats Hub::stats(const int link) const {
    LinkStats stats;
    if (link < 0 || link >= max_links)
        return stats;
    const Link& entry = links_[static_cast<size_t>(link)];
    stats.reports = entry.reports.load(std::memory_order_relaxed);
    stats.merged = entry.merged.load(std::memory_order_relaxed);
    stats.queue_max = entry.queue_worst.load(std::memory_order_relaxed);
    stats.total_max = entry.total_worst.load(std::memory_order_relaxed);
    stats.realtime = entry.realtime.load(std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> lock(entry.queue);
        stats.depth = static_cast<uint32_t>(entry.count + entry.jumps.size() +
                                            (entry.motion.any() ? 1U : 0U));
    }
    const size_t taken = static_cast<size_t>(
        std::min<uint64_t>(entry.ring_next.load(std::memory_order_relaxed), ring_size));
    std::vector<uint32_t> queued(taken);
    std::vector<uint32_t> total(taken);
    for (size_t at = 0; at < taken; ++at) {
        queued[at] = entry.queue_us[at].load(std::memory_order_relaxed);
        total[at] = entry.total_us[at].load(std::memory_order_relaxed);
    }
    std::sort(queued.begin(), queued.end());
    std::sort(total.begin(), total.end());
    stats.samples = static_cast<uint32_t>(taken);
    stats.queue_p50 = percentile(queued, 50);
    stats.queue_p99 = percentile(queued, 99);
    stats.total_p50 = percentile(total, 50);
    stats.total_p99 = percentile(total, 99);
    return stats;
}

Sink& Hub::sink(const int link) noexcept {
    return links_[static_cast<size_t>(std::clamp(link, 0, max_links - 1))].sink;
}

int Hub::start_proxy(const int link, const uint16_t port, bool& held, bool& absent) {
    held = absent = false;
    if (link < 0 || link >= max_links)
        return proxy_refused;
    Link& entry = links_[static_cast<size_t>(link)];
    const std::lock_guard<std::mutex> lock(entry.port_lock);
    if (entry.port == nullptr)
        return proxy_refused;
    return entry.port->start_proxy(port, held, absent);
}

void Hub::stop_proxy(const int link) {
    if (link < 0 || link >= max_links)
        return;
    Link& entry = links_[static_cast<size_t>(link)];
    const std::lock_guard<std::mutex> lock(entry.port_lock);
    if (entry.port != nullptr)
        entry.port->stop_proxy();
}


void Hub::LinkSink::move(const Delta counts) {
    Link& link = hub_->links_[link_];
    const int64_t at = now_ns();
    {
        const std::lock_guard<std::mutex> lock(link.queue);
        // Behind a waiting key or button it is summed after all, and goes
        // out with that edge: the two queues keep no order between them.
        if (link.every.load(std::memory_order_relaxed) && link.count == 0) {
            queue_alone(link, Motion{counts.x, counts.y, 0, 0, at});
        } else {
            Motion& motion = link.motion;
            if (motion.any())
                link.merged.fetch_add(1, std::memory_order_relaxed);
            else
                motion.at = at;
            motion.dx += counts.x;
            motion.dy += counts.y;
        }
    }
    link.wake.notify_one();
}

void Hub::LinkSink::jump(const Delta counts) {
    Link& link = hub_->links_[link_];
    const int64_t at = now_ns();
    {
        const std::lock_guard<std::mutex> lock(link.queue);
        queue_alone(link, Motion{counts.x, counts.y, 0, 0, at});
    }
    link.wake.notify_one();
}

void Hub::queue_alone(Link& link, const Motion& report) {
    // What was moved before it stays before it.
    if (link.motion.any()) {
        link.jumps.push_back(link.motion);
        link.motion = {};
    }
    link.jumps.push_back(report);
}

void Hub::LinkSink::scroll(const int wheel, const int pan) {
    Link& link = hub_->links_[link_];
    const int64_t at = now_ns();
    {
        const std::lock_guard<std::mutex> lock(link.queue);
        if (link.every.load(std::memory_order_relaxed) && link.count == 0) {
            queue_alone(link, Motion{0, 0, wheel, pan, at});
        } else {
            Motion& motion = link.motion;
            if (motion.any())
                link.merged.fetch_add(1, std::memory_order_relaxed);
            else
                motion.at = at;
            motion.wheel += wheel;
            motion.pan += pan;
        }
    }
    link.wake.notify_one();
}

void Hub::LinkSink::button(const unsigned button, const bool down) {
    hub_->push(hub_->links_[link_],
               {event_button, link_, static_cast<uint16_t>(button), down, now_ns()});
}

void Hub::LinkSink::key(const uint16_t usage, const bool down) {
    hub_->push(hub_->links_[link_], {event_key, link_, usage, down, now_ns()});
}

void Hub::LinkSink::media(const uint16_t usage, const bool down) {
    hub_->push(hub_->links_[link_], {event_media, link_, usage, down, now_ns()});
}

void Hub::LinkSink::touch(const unsigned contact, const int x, const int y, const bool down) {
    if (contact > 1)
        return;
    Link& link = hub_->links_[link_];
    {
        const std::lock_guard<std::mutex> lock(link.queue);
        // A lift ends the swipe, where its finger is.
        if (!down && contact == 1 && link.swipe.active) {
            link.swipe.active = false;
            hub_->queue_touch_locked(link, contact, static_cast<int>(std::lround(link.swipe.x)),
                                     static_cast<int>(std::lround(link.swipe.y)), false, now_ns());
        } else {
            hub_->queue_touch_locked(link, contact, x, y, down, now_ns());
        }
    }
    link.wake.notify_one();
}

void Hub::LinkSink::swipe(const SwipePlan& plan) {
    Link& link = hub_->links_[link_];
    const auto seconds = [](const double value) {
        return static_cast<int64_t>(std::max(value, 0.0) * 1e9);
    };
    const auto inside = [&](const double value, const int size) {
        return std::clamp(value, 0.0, std::max(size - 1.0, 0.0));
    };
    {
        const std::lock_guard<std::mutex> lock(link.queue);
        Link::Swipe& swipe = link.swipe;
        const int64_t now = now_ns();
        if (swipe.active && plan.restart) {
            swipe.active = false;
            hub_->queue_touch_locked(link, 1, static_cast<int>(std::lround(swipe.x)),
                                     static_cast<int>(std::lround(swipe.y)), false, now);
        }
        swipe.area = plan.area;
        if (!swipe.active) {
            // Put down where the cursor is; the first move comes after the start time.
            swipe.active = true;
            swipe.x = plan.x;
            swipe.y = plan.y;
            swipe.to_x = inside(plan.x + plan.dx, plan.area.w);
            swipe.to_y = inside(plan.y + plan.dy, plan.area.h);
            swipe.next_at = now + seconds(plan.start_s);
            hub_->queue_touch_locked(link, 1, plan.x, plan.y, true, now, 0);
        } else {
            // Same finger: what is left grows by the notch, and the moves go on at once.
            swipe.to_x = inside(swipe.to_x + plan.dx, plan.area.w);
            swipe.to_y = inside(swipe.to_y + plan.dy, plan.area.h);
            swipe.next_at = now;
        }
        const int steps = std::max(plan.steps, 1);
        swipe.steps_left = steps;
        swipe.gap = steps > 1 ? seconds(plan.total_s) / (steps - 1) : 0;
        swipe.release = seconds(plan.release_s);
        swipe.lift_at = 0;
    }
    link.wake.notify_one();
}

void Hub::queue_touch_locked(Link& link, const unsigned contact, const int x, const int y,
                             const bool down, const int64_t at, const int64_t gap_ns) {
    // Only a move of the first contact merges with a move (a fast drag must
    // not fill the queue); the event that puts a contact down stays an event
    // of its own, so the placement is sent before the move.
    if (down && contact == 0 && link.count != 0) {
        Event& last = link.events[(link.head + link.count - 1) % link.events.size()];
        if (last.kind == event_touch && last.code == contact && last.down && !last.placing) {
            last.x = x;
            last.y = y;
            return;
        }
    }
    // While a lift waits for room, nothing new is put down: it would be
    // lifted by that lift.
    if (down && link.lift_owed[contact])
        return;
    if (link.count == link.events.size()) {
        // A full queue means the phone stopped answering, and the link fails
        // on its own timeout, so the newest edge is dropped. A lift is kept,
        // or the contact would stay down on the device.
        if (!down) {
            link.lift_owed[contact] = true;
            link.lift_x[contact] = x;
            link.lift_y[contact] = y;
            link.touch_queued_down[contact] = false;
        }
        return;
    }
    Event& slot = link.events[(link.head + link.count) % link.events.size()];
    slot = Event{event_touch,
                 static_cast<uint8_t>(&link - links_.data()),
                 static_cast<uint16_t>(contact),
                 down,
                 at,
                 x,
                 y,
                 down && !link.touch_queued_down[contact],
                 gap_ns};
    link.touch_queued_down[contact] = down;
    ++link.count;
}

void Hub::push(Link& link, const Event& event) {
    {
        const std::lock_guard<std::mutex> lock(link.queue);
        // A full queue means the phone stopped answering; the link fails on
        // its own timeout, so the newest edge is dropped here.
        if (link.count == link.events.size())
            return;
        link.events[(link.head + link.count) % link.events.size()] = event;
        ++link.count;
    }
    link.wake.notify_one();
}

void Hub::run(Link& link) {
    using clock = std::chrono::steady_clock;
    link.realtime.store(favour_this_thread(), std::memory_order_relaxed);
    clock::time_point next_tick = clock::now();
    clock::time_point placed_until = next_tick;
    std::unique_lock<std::mutex> lock(link.queue);
    // Android drops a touch when a mouse report arrives while it is down, and
    // takes that for a cancel, not a lift. So movement waits while a contact
    // is down or a touch event is still queued, and goes out in a report
    // after the lift.
    const auto touching = [&] {
        if (link.swipe.active || link.touch_queued_down[0] || link.touch_queued_down[1])
            return true;
        for (size_t waiting = 0; waiting < link.count; ++waiting) {
            if (link.events[(link.head + waiting) % link.events.size()].kind == event_touch)
                return true;
        }
        return false;
    };
    const auto pending = [&] {
        return link.count != 0 || !link.jumps.empty() || (link.motion.any() && !touching());
    };
    for (;;) {
        for (;;) {
            // The swipe's moves and its lift go in on their times, like any
            // other edge: one move per pass, so they are not summed.
            const int64_t now = now_ns();
            int64_t next = 0;
            Link::Swipe& swipe = link.swipe;
            if (swipe.active) {
                const auto due = [&](const int64_t at) {
                    if (next == 0 || at < next)
                        next = at;
                };
                const auto at_finger = [&](const bool down) {
                    queue_touch_locked(link, 1, static_cast<int>(std::lround(swipe.x)),
                                       static_cast<int>(std::lround(swipe.y)), down, now);
                };
                if (swipe.steps_left > 0) {
                    if (now >= swipe.next_at) {
                        swipe.x += (swipe.to_x - swipe.x) / swipe.steps_left;
                        swipe.y += (swipe.to_y - swipe.y) / swipe.steps_left;
                        --swipe.steps_left;
                        at_finger(true);
                        if (swipe.steps_left > 0)
                            swipe.next_at = now + swipe.gap;
                        else
                            swipe.lift_at = now + swipe.release;
                    } else {
                        due(swipe.next_at);
                    }
                }
                if (swipe.steps_left == 0 && swipe.lift_at != 0) {
                    if (now >= swipe.lift_at) {
                        swipe.active = false;
                        at_finger(false);
                    } else {
                        due(swipe.lift_at);
                    }
                }
            }
            // A lift that found the queue full goes in as soon as there is room.
            for (size_t contact = 0; contact < link.lift_owed.size(); ++contact) {
                if (link.lift_owed[contact] && link.count < link.events.size()) {
                    link.lift_owed[contact] = false;
                    queue_touch_locked(link, static_cast<unsigned>(contact), link.lift_x[contact],
                                       link.lift_y[contact], false, now);
                }
            }
            if (link.stopping || pending())
                break;
            if (next == 0)
                link.wake.wait(lock);
            else
                link.wake.wait_until(lock, clock::time_point(std::chrono::nanoseconds(next)));
        }
        if (link.stopping)
            return;

        // A jump goes out alone and first: Android has to clamp it before
        // the movement queued behind it is applied.
        if (!link.jumps.empty()) {
            const Motion jump = link.jumps.front();
            link.jumps.pop_front();
            lock.unlock();
            deliver(link, jump, nullptr, 0);
            lock.lock();
            continue;
        }

        // Motion alone waits for the next tick of the configured rate; a key
        // or button edge goes out at once and takes the pending motion along.
        const unsigned rate = link.rate_hz.load(std::memory_order_relaxed);
        // What follows a finger's placement waits one report period (1 ms at
        // least), so Android sees the finger down before it moves.
        if (clock::now() < placed_until) {
            link.wake.wait_until(lock, placed_until,
                                 [&] { return link.stopping || !link.jumps.empty(); });
            if (link.stopping)
                return;
            if (!link.jumps.empty())
                continue;
        }
        if (rate != 0 && link.count == 0) {
            const auto period = std::chrono::nanoseconds(1000000000LL / rate);
            const clock::time_point now = clock::now();
            if (next_tick > now) {
                // A key, a button or a jump ends the wait: none of them is paced.
                link.wake.wait_until(lock, next_tick, [&] {
                    return link.stopping || link.count != 0 || !link.jumps.empty();
                });
                if (link.stopping)
                    return;
                if (!link.jumps.empty())
                    continue;
            }
            next_tick = std::max(next_tick, clock::now() - period) + period;
        }
        if (!pending())
            continue;

        // A report holds one state per key and button, so a second edge of
        // the same one waits for the next report.
        std::array<Event, 32> batch{};
        size_t taken = 0;
        while (link.count != 0 && taken < batch.size()) {
            const Event& front = link.events[link.head];
            const bool repeat =
                std::any_of(batch.begin(), batch.begin() + static_cast<std::ptrdiff_t>(taken),
                            [&](const Event& earlier) {
                                return earlier.kind == front.kind &&
                                       (earlier.code == front.code || front.kind == event_media);
                            });
            if (repeat)
                break;
            batch[taken++] = front;
            link.head = (link.head + 1) % link.events.size();
            --link.count;
        }

        // The batch's own touch events are no longer queued, but they are
        // not yet delivered: movement still waits for them.
        Motion motion;
        const bool batch_touches =
            std::any_of(batch.begin(), batch.begin() + static_cast<std::ptrdiff_t>(taken),
                        [](const Event& each) { return each.kind == event_touch; });
        if (!batch_touches && !touching()) {
            motion = link.motion;
            link.motion = {};
        }
        // A finger that was put down holds the next report back: by what its
        // event says, or one report period (1 ms at least).
        int64_t wait_ns = 0;
        for (size_t index = 0; index < taken; ++index) {
            const Event& each = batch[index];
            if (each.kind != event_touch || !each.placing)
                continue;
            const int64_t period =
                rate != 0 ? 1000000000LL / static_cast<int64_t>(rate) : int64_t{0};
            wait_ns = std::max(wait_ns,
                               each.gap_ns >= 0 ? each.gap_ns : std::max(period, int64_t{1000000}));
        }
        lock.unlock();
        deliver(link, motion, batch.data(), taken);
        lock.lock();
        if (wait_ns > 0)
            placed_until = clock::now() + std::chrono::nanoseconds(wait_ns);
    }
}

void Hub::fail(Link& link, const std::string& why) {
    {
        const std::lock_guard<std::mutex> lock(link.note);
        link.failure = why;
    }
    link.failed.store(true, std::memory_order_release);
}

void Hub::deliver(Link& link, const Motion& motion, const Event* events, const size_t count) {
    const std::lock_guard<std::mutex> lock(link.port_lock);
    if (!link.open.load(std::memory_order_acquire) || link.failed.load(std::memory_order_acquire))
        return;
    Port& port = *link.port;

    // Of everything this report carries, what came first sets its age.
    int64_t first = motion.at;
    for (size_t index = 0; index < count; ++index) {
        if (events[index].at != 0 && (first == 0 || events[index].at < first))
            first = events[index].at;
    }
    const int64_t started = now_ns();

    // A Node that is absent, or was added a moment ago, takes nothing.
    const auto usable = [&](const NodeKind kind) {
        return port.has(kind) && started >= link.node_ready_ns[kind];
    };
    const bool can_mouse = usable(node_mouse);
    const bool can_keyboard = usable(node_keyboard);
    const bool can_media = usable(node_media);
    const bool can_touch = usable(node_touch);

    // The Nodes this report touched.
    uint8_t touched = 0;
    const auto mark = [&](const NodeKind kind) {
        touched = static_cast<uint8_t>(touched | node_bit(kind));
    };
    bool ok = true;
    // What this report carries, kept only while someone is looking.
    const bool tracing = started < link.trace_until.load(std::memory_order_relaxed);
    std::array<Trace, 34> calls;
    size_t called = 0;
    const auto note_call = [&](const Trace::Kind kind, const uint8_t phase, const int32_t a,
                               const int32_t b = 0, const int32_t c = 0) {
        if (tracing && called < calls.size())
            calls[called++] = Trace{started, 0, kind, phase, a, b, c};
    };
    if (can_mouse && (motion.dx != 0 || motion.dy != 0)) {
        ok = port.mouse_move(narrow(motion.dx), narrow(motion.dy));
        note_call(Trace::mouse_move, 0, narrow(motion.dx), narrow(motion.dy));
        mark(node_mouse);
    }
    if (can_mouse && ok && (motion.wheel != 0 || motion.pan != 0)) {
        ok = port.mouse_scroll(narrow(motion.wheel), narrow(motion.pan));
        note_call(Trace::mouse_scroll, 0, narrow(motion.wheel), narrow(motion.pan));
        mark(node_mouse);
    }
    for (size_t index = 0; index < count && ok; ++index) {
        const Event& event = events[index];
        switch (event.kind) {
        case event_button:
            if (can_mouse) {
                ok = port.button(event.code, event.down);
                note_call(Trace::button, event.down, event.code);
                mark(node_mouse);
            }
            break;
        case event_key:
            if (can_keyboard) {
                ok = port.key(event.code, event.down);
                note_call(Trace::key, event.down, event.code);
                mark(node_keyboard);
            }
            break;
        case event_media:
            if (can_media) {
                ok = port.media(event.code, event.down);
                note_call(Trace::media, event.down, event.code);
                mark(node_media);
            }
            break;
        case event_touch:
            if (can_touch && event.code < 2) {
                // A lift of a contact that was never placed (its down was
                // dropped while the Node settled) is not sent: the library
                // rejects it and the link would fail.
                if (!event.down && !link.touch_down[event.code])
                    break;
                ok = port.touch(event.code, event.down, event.x, event.y);
                note_call(Trace::touch,
                          !event.down                   ? 0
                          : link.touch_down[event.code] ? 2
                                                        : 1,
                          event.code, event.x, event.y);
                link.touch_down[event.code] = event.down;
                mark(node_touch);
            }
            break;
        default:
            break;
        }
    }

    if (ok && touched != 0)
        ok = port.flush(touched);
    if (!ok) {
        fail(link, port.error());
        return;
    }
    const uint64_t report = link.reports.fetch_add(1, std::memory_order_relaxed) + 1;
    if (called != 0) {
        const std::lock_guard<std::mutex> lock(link.trace_lock);
        for (size_t index = 0; index < called; ++index) {
            calls[index].report = report;
            link.trace_ring[link.trace_next++ % link.trace_ring.size()] = calls[index];
        }
    }
    if (first == 0)
        return;
    const int64_t done = now_ns();
    const size_t slot = link.ring_next.fetch_add(1, std::memory_order_relaxed) % ring_size;
    const uint32_t queued = micros(first, started);
    const uint32_t total = micros(first, done);
    link.queue_us[slot].store(queued, std::memory_order_relaxed);
    link.total_us[slot].store(total, std::memory_order_relaxed);
    note_worst(link.queue_worst, queued);
    note_worst(link.total_worst, total);
}

} // namespace hids
