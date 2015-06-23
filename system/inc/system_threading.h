/**7
  Copyright (c) 2013-2015 Particle Industries, Inc.  All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation, either
  version 3 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, see <http://www.gnu.org/licenses/>.
  ******************************************************************************
 */

#ifndef SYSTEM_THREADING_H
#define	SYSTEM_THREADING_H

#include "concurrent_hal.h"
#include "channel.h"
#include <stddef.h>
#include <mutex>
#include <functional>
#include <future>

#ifndef PARTICLE_GTHREAD_INCLUDED
#error "GTHREAD header not included. This is required for correct mutex implementation on embedded platforms."
#endif

struct ActiveObjectConfiguration
{
    /**
     * Function to call when there are no objects to process in the queue.
     */
    typedef std::function<void(void)> background_task_t;

    /**
     * The function to run when there is nothing else to do.
     */
    background_task_t background_task;
    size_t stack_size;


public:
    ActiveObjectConfiguration(background_task_t task, size_t stack_size_) : background_task(task), stack_size(stack_size_) {}

};


class ActiveObjectBase
{
public:

protected:

    ActiveObjectConfiguration configuration;

    /**
     * The thread that runs this active object.
     */
    std::thread _thread;

    std::mutex _start;

    volatile bool started;

    /**
     * The main run loop for an active object.
     */
    void run();


    void invoke_impl(void* fn, void* data, size_t len=0);

protected:

    /**
     * Describes a monadic function to be called by the active object.
     */
    struct Item
    {
        typedef void (*active_fn_t)(void*);
        typedef std::function<void(void)> task_t;

        active_fn_t function;
        void* arg;              // the argument is dynamically allocated

        task_t task;

        Item() : function(NULL), arg(NULL){}
        Item(active_fn_t f, void* a) : function(f), arg(a){}
        Item(task_t&& _task, void* _arg=NULL) : arg(_arg), task(_task) {}


        void invoke()
        {
            if (function) {
                function(arg);
            }
        }

        void dispose()
        {
            free(arg);
            function = NULL;
            arg = NULL;
        }
    };

    // todo - concurrent queue should be a strategy so it's pluggable without requiring inheritance
    virtual bool take(Item& item)=0;
    virtual void put(const Item& item)=0;

    void set_thread(std::thread&& thread)
    {
        this->_thread.swap(thread);
    }

    /**
     * Static thread entrypoint to run this active object loop.
     * @param obj
     */
    static void run_active_object(ActiveObjectBase* obj);

    void start_thread();

public:

    ActiveObjectBase(const ActiveObjectConfiguration& config) : configuration(config), started(false) {}

    bool isCurrentThread() {
        __gthread_t thread_handle = _thread.native_handle();
        __gthread_t current = __gthread_self();
        return __gthread_equal(thread_handle, current);
    }

    bool isStarted() {
        return started;
    }

    template<typename arg, typename r> inline void invoke(r (*fn)(arg* a), arg* value, size_t size) {
        invoke_impl((void*)fn, value, size);
    }

    template<typename arg, typename r> inline void invoke(r (*fn)(arg* a), arg* value) {
        invoke_impl((void*)fn, value);
    }

    void invoke(void (*fn)()) {
        invoke_impl((void*)fn, NULL, 0);
    }

    template<typename R> std::future<R> invoke_future(std::function<R(void)> fn) {
        std::packaged_task<R(void)> task(fn);
        //put(Item(task));
        return task.get_future();
    }

};


template <size_t queue_size=50>
class ActiveObjectChannel : public ActiveObjectBase
{
    cpp::channel<Item, queue_size> _channel;

protected:

    virtual bool take(Item& item)
    {
        return cpp::select().recv_only(_channel, item).try_once();
    }

    virtual void put(const Item& item)
    {
        _channel.send(item);
    }


public:

    ActiveObjectChannel(ActiveObjectConfiguration& config) : ActiveObjectBase(config) {}

    /**
     * Start the asynchronous processing for this active object.
     */
    void start()
    {
        _channel = cpp::channel<Item, queue_size>();
        start_thread();
    }

};


class ActiveObjectQueue : public ActiveObjectBase
{
    os_queue_t  queue;

protected:

    virtual bool take(Item& item)
    {
        return os_queue_take(queue, &item, 100)==0;
    }

    virtual void put(const Item& item)
    {
        while (!os_queue_put(queue, &item, CONCURRENT_WAIT_FOREVER)) {}
    }

public:

    ActiveObjectQueue(const ActiveObjectConfiguration& config) : ActiveObjectBase(config), queue(NULL) {}

    void start()
    {
        os_queue_create(&queue, sizeof(Item), 50);
        start_thread();
    }

};

typedef ActiveObjectQueue ActiveObject;

extern ActiveObject SystemThread;
extern ActiveObject AppThread;


// execute the enclosing void function async on the system thread
#define SYSTEM_THREAD_CONTEXT_RESULT(result) \
    if (SystemThread.isStarted() && !SystemThread.isCurrentThread()) {\
        SYSTEM_THREAD_CONTEXT_FN0(__func__); \
        return result; \
    }

#define SYSTEM_THREAD_CONTEXT(result)


#define SYSTEM_THREAD_CONTEXT_FN0(fn) \
    SystemThread.invoke(fn);


#define SYSTEM_THREAD_CONTEXT_FN1(fn, arg, sz) \
    SystemThread.invoke(fn, arg, sz)


template<typename T>
struct memfun_type
{
    using type = void;
};

template<typename Ret, typename Class, typename... Args>
struct memfun_type<Ret(Class::*)(Args...) const>
{
    using type = std::function<Ret(Args...)>;
};

template<typename F>
typename memfun_type<decltype(&F::operator())>::type
FFL(F const &func)
{ // Function from lambda !
    return func;
}


// execute synchrnously on the system thread. Since the parameter lifetime is
// assumed to be bound by the caller, the parameters don't need marshalling
// fn: the function call to perform. This is textually substitued into a lambda, with the
// parameters passed by copy.
#define SYSTEM_THREAD_CONTEXT_SYNC(fn) \
    if (SystemThread.isStarted() && !SystemThread.isCurrentThread()) { \
        auto lambda = [=]() { return (fn); }; \
        auto future = SystemThread.invoke_future(FFL(lambda)); \
        future.wait(); \
        return future.get(); \
    }



#endif	/* SYSTEM_THREADING_H */

