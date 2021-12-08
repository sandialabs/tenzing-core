#pragma once

/*! \file Model an execution platform
*/

#include <map>

#include <mpi.h>
#include <nlohmann/json.hpp>

#include "cuda_runtime.h"
#include "macro_at.hpp"

/* handle representing a CUDA stream
*/
struct Stream {

    typedef unsigned id_t;

    id_t id_;
    Stream(id_t id) : id_(id) {}
    Stream() : Stream(0) {} // default stream

    bool operator<(const Stream &rhs) const {
        return id_ < rhs.id_;
    }
    bool operator>(const Stream &rhs) const {
        return id_ > rhs.id_;
    }
    bool operator==(const Stream &rhs) const {
        return id_ == rhs.id_;
    }
    bool operator!=(const Stream &rhs) const {
        return !(rhs == *this);
    }
};


void to_json(nlohmann::json& j, const Stream &s);
void from_json(const nlohmann::json& j, Stream &s);

inline std::ostream &operator<<(std::ostream &os, const Stream &s) {
    os << s.id_;
    return os;
}

/* handle representing a CUDA event
*/
struct Event {

    typedef unsigned id_t;

    id_t id_;
    Event(id_t id) : id_(id) {}
    Event() : Event(0) {} // default Event
    explicit operator id_t() const {return id_;}

    bool operator<(const Event &rhs) const {
        return id_ < rhs.id_;
    }
    bool operator>(const Event &rhs) const {
        return id_ > rhs.id_;
    }
    bool operator==(const Event &rhs) const {
        return id_ == rhs.id_;
    }
    bool operator!=(const Event &rhs) const {
        return !(rhs == *this);
    }
};


void to_json(nlohmann::json& j, const Event &s);
void from_json(const nlohmann::json& j, Event &s);

inline std::ostream &operator<<(std::ostream &os, const Event &s) {
    os << s.id_;
    return os;
}

struct CPU {
    int id_;
    CPU(int id) : id_(id) {}
    operator int() const {return id_;}
};


struct Platform {

private:
    size_t iEvent_; // pointer to next event from new_event()
    std::vector<cudaStream_t> cStreams_;
    std::vector<cudaEvent_t> events_;
    MPI_Comm comm_;
public:

    std::vector<Stream> streams_;

    Platform(MPI_Comm comm) : iEvent_(0), comm_(comm) {
        // default stream
        streams_.push_back(Stream(0));
        cStreams_.push_back(0);
    }
    ~Platform() {
        for (auto &event : events_) {
            CUDA_RUNTIME(cudaEventDestroy(event));
        }
        // don't try to delete default stream
        for (size_t i = 1; i < cStreams_.size(); ++i) {
            CUDA_RUNTIME(cudaStreamDestroy(cStreams_[i]));
        }
    }
    Platform(const Platform &other) = delete; // stream lifetime?
    Platform(Platform &&other) = default;

    void reset_dynamic_resources() {
        iEvent_ = 0;
    }


    // return the number of streams, not counting the default stream
    int num_streams() const {
        if (streams_.size() != cStreams_.size()) {
            THROW_RUNTIME("internal platform stream bookkeeping error");
        }
        if (streams_.empty()) {
            THROW_RUNTIME("platform missing default stream")
        }
        return streams_.size() - 1;
    }

    int num_events() const {
        return iEvent_;
    }

    cudaStream_t cuda_stream(const Stream &stream) const {
        if (UNLIKELY(stream.id_ >= streams_.size())) {
            THROW_RUNTIME("requested non-existent stream " << stream.id_);
        }
        return cStreams_[stream.id_];
    }

    cudaEvent_t cuda_event(const Event &event) const {
        if (UNLIKELY(event.id_ >= events_.size())) {
            THROW_RUNTIME("requested unreserved event " << event.id_);
        }
        if (UNLIKELY(event.id_ >= iEvent_)) {
            THROW_RUNTIME("requested invalid event handle " << event.id_);
        }
        return events_[event.id_];
    }

    Event new_event() {
        reserve_events(iEvent_+1);
        return Event(iEvent_++);
    }

    Stream new_stream() {
        Stream stream(streams_.size());
        streams_.push_back(stream);
        cudaStream_t s;
        CUDA_RUNTIME(cudaStreamCreate(&s));
        cStreams_.push_back(s);
        return stream;
    }

    const MPI_Comm &comm() const {
        return comm_;
    }
    MPI_Comm &comm() {
        return comm_;
    }


    void ensure_streams(int n) {
        while(num_streams() < n) {
            new_stream();
        }
    }

    // ensure there are n backing CUDA events
    void reserve_events(size_t n) {
        while(events_.size() < n) {
            cudaEvent_t e;
            CUDA_RUNTIME(cudaEventCreateWithFlags(&e, cudaEventDisableTiming));
            events_.push_back(e);            
        }
    }

    // create event handles for `n` events
    void ensure_events(size_t n) {
        reserve_events(n);
        if (iEvent_ < n) iEvent_ = n;
    }

    //create a platform with `n` streams
    static Platform make_n_streams(int n, MPI_Comm comm) {
        Platform ret(comm);
        ret.ensure_streams(n);
        return ret;
    }



};

class IValue {
    virtual ~IValue() {}
    virtual size_t size_bytes() = 0;
    virtual size_t align() = 0;
    virtual size_t elem_bytes() = 0;
    virtual size_t elem_count() = 0;
};

template <typename T>
class ScalarValue : public IValue {
public:
    size_t size_bytes() const override {return sizeof(T); }
    size_t align() const override {return alignof(T); }
    size_t elem_bytes() const override {return sizeof(T); }
    size_t elem_count() const {return 1; }
};

template <typename T>
class ArrayValue : public IValue {
protected:
    size_t count_;
public:
    size_t size_bytes() const override {return count_ * sizeof(T); }
    size_t align() const override {return alignof(T); }
    size_t elem_bytes() const override {return sizeof(T); }
    size_t elem_count() const {return count_; }
};

class ResourceMap {
    std::map<Event, cudaEvent_t> events;
    std::map<IValue *, void *> addrs;

    public:
    bool contains(const Event &event) {
        return 0 != events.count(event);
    }
    bool insert(const Event &event, cudaEvent_t cevent) {
        return events.insert(std::make_pair(event, cevent)).second;
    }
};


class CudaEventPool {
    std::vector<cudaEvent_t> events_;
    size_t i_;
public:

    CudaEventPool() : i_(0) {}
    ~CudaEventPool() {
        for (cudaEvent_t &e : events_) {
            CUDA_RUNTIME(cudaEventDestroy(e));
        }
    }
    CudaEventPool(CudaEventPool &&other) noexcept = default;
    CudaEventPool(const CudaEventPool &rhs) = delete;
    CudaEventPool &operator=(const CudaEventPool &other) = delete;
    CudaEventPool &operator=(CudaEventPool &&other) noexcept = delete;


    cudaEvent_t new_event() {
        while (i_ <= events_.size()) {
            cudaEvent_t e;
            CUDA_RUNTIME(cudaEventCreateWithFlags(&e, cudaEventDisableTiming));
            events_.push_back(e);
        }
        return events_[i_++];       
    };

    void reset() { i_ = 0; }

};