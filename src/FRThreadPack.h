#ifndef HTCW_FRTHREADPACK_H
#define HTCW_FRTHREADPACK_H
#include <atomic>
#include <functional>
#include "freertos/task.h"
#include "freertos/ringbuf.h"
// provides an easy way to synchronize access between threads
class FRSynchronizationContext
{
    bool m_owned;
    RingbufHandle_t m_messageRingBufferHandle;
    struct Message
    {
        std::function<void(void *)> callback;
        void *state;
        TaskHandle_t finishedNotifyHandle;
    };

public:
    // creates the synchronization context. the queueSize parameter indicates the number of methods in the queue before it blocks
    FRSynchronizationContext(size_t queueSize = 10) : m_owned(true),m_messageRingBufferHandle(nullptr) {
        m_messageRingBufferHandle = xRingbufferCreate(sizeof(Message) * queueSize + (sizeof(Message) - 1), RINGBUF_TYPE_NOSPLIT);
        if(nullptr==m_messageRingBufferHandle)
            m_owned=false;
    }
    FRSynchronizationContext(const FRSynchronizationContext &rhs) : m_owned(false),m_messageRingBufferHandle(rhs.m_messageRingBufferHandle) {
        
    }
    FRSynchronizationContext(FRSynchronizationContext &&rhs) : m_owned(rhs.m_owned), m_messageRingBufferHandle(rhs.m_messageRingBufferHandle) {
        rhs.m_messageRingBufferHandle = nullptr;
        rhs.m_owned=false;
    }
    FRSynchronizationContext &operator=(const FRSynchronizationContext &rhs){
        m_messageRingBufferHandle=rhs.m_messageRingBufferHandle;
        m_owned=false;
        return *this;
    }
    FRSynchronizationContext &operator=(FRSynchronizationContext &&rhs) {
        m_messageRingBufferHandle=rhs.m_messageRingBufferHandle;
        m_owned=rhs.m_owned;
        rhs.m_messageRingBufferHandle=nullptr;
        rhs.m_owned=false;
        return *this;
    }
    // destroys the synchronization context
    virtual ~FRSynchronizationContext()
    {
        if (nullptr != m_messageRingBufferHandle)
        {
            if(m_owned) {
                vRingbufferDelete(m_messageRingBufferHandle);
            }
            m_messageRingBufferHandle = nullptr;
        }
        m_owned=false;
    }
    // indicates the handle of the synchronization context
    RingbufHandle_t handle() {
        return m_messageRingBufferHandle;
    }
    // posts a message to the thread update() is called from. this method does not block
    bool post(std::function<void(void *)> fn, void *state = nullptr, uint32_t timeoutMS = 10000)
    {
        if(nullptr!=m_messageRingBufferHandle) {
            Message msg;
            msg.callback = fn;
            msg.state = state;
            msg.finishedNotifyHandle = nullptr;
            UBaseType_t res = xRingbufferSend(m_messageRingBufferHandle, &msg, sizeof(msg), pdMS_TO_TICKS(timeoutMS));
            return (res == pdTRUE);
        }
        return false;
    }
    // sends a message to the thread update() is called from. this method blocks until the update thread executes the method and it returns.
    bool send(std::function<void(void *)> fn, void *state = nullptr, uint32_t timeoutMS = 10000)
    {
        if(nullptr!=m_messageRingBufferHandle) {
            Message msg;
            msg.callback = fn;
            msg.state = state;
            msg.finishedNotifyHandle = xTaskGetCurrentTaskHandle();
            uint32_t mss = millis();
            UBaseType_t res = xRingbufferSend(m_messageRingBufferHandle, &msg, sizeof(msg), pdMS_TO_TICKS(timeoutMS));
            mss = millis() - mss;
            if (timeoutMS >= mss)
                timeoutMS -= mss;
            else
                timeoutMS = 0;
            if (res == pdTRUE)
            {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeoutMS));
                return true;
            }
        }
        return false;
    }
    // posts the special quit message to the synchronization context
    bool postQuit() {
        return post(std::function<void(void*)>(),nullptr);
    }
    // ensures that the lifetime of the object is not tied to this destructor
    void detach() {
        m_owned=false;
    }
    // processes a pending message in the message queue. This should be called in a loop on the target thread.
    bool processOne(bool blockUntilReady=false)
    {
        if(nullptr!=m_messageRingBufferHandle) {
            //Receive an item from no-split ring buffer
            size_t size = sizeof(Message);
            Message *pmsg = (Message *)xRingbufferReceive(m_messageRingBufferHandle, &size,blockUntilReady?portMAX_DELAY:0);
            if (nullptr == pmsg)
                return true;
            if (size != sizeof(Message))
                return false;
            Message msg = *pmsg;
            vRingbufferReturnItem(m_messageRingBufferHandle, pmsg);    
            if(msg.callback) {
                msg.callback(msg.state);
                if (nullptr != msg.finishedNotifyHandle)
                {
                    xTaskNotifyGive(msg.finishedNotifyHandle);
                }
                return true;
            }
        }
        return false;
    }
};
// used by the FRThreadPool framework. Not intended for user code
struct FRThreadPoolCreationData_t {
    TaskHandle_t* ptaskHandle;
    FRSynchronizationContext* psyncContext;
    std::atomic<size_t>* pthreadCount;
};
class FRThread {
    TaskHandle_t m_handle;
    private:
        struct TaskEntryThunk_t {
            TaskHandle_t callingThreadHandle;
            const void* state;
            std::function<void(const void*)> fn;
        };
        struct DispatcherEntry_t {
            TaskHandle_t callingThreadHandle;
            TaskHandle_t* pshutdownThreadHandle;
            FRSynchronizationContext* psyncContext;
            std::atomic<size_t>* pthreadCount;

        };
        static void taskEntryThunk(void *ptr) {
            TaskEntryThunk_t thunk = *((TaskEntryThunk_t*)ptr);
            // let the creator know we're done
            xTaskNotifyGive(thunk.callingThreadHandle);
            vTaskSuspend(nullptr);
            thunk.fn(thunk.state);
            vTaskDelete(nullptr);
        }
        static void dispatcherEntry(void* ptr) {
            DispatcherEntry_t de = *((DispatcherEntry_t*)ptr);
            FRSynchronizationContext sc = *((FRSynchronizationContext*)de.psyncContext);
            if(nullptr!=de.pthreadCount)
                ++*de.pthreadCount;
            if(nullptr!=de.callingThreadHandle)
                xTaskNotifyGive(de.callingThreadHandle);
            while(sc.processOne(true));
            if(nullptr!=de.pshutdownThreadHandle && nullptr!=*de.pshutdownThreadHandle) {
                xTaskNotifyGive(*de.pshutdownThreadHandle);
            }
            if(nullptr!=de.pthreadCount)
                --*de.pthreadCount;
            vTaskDelete(nullptr);
        }
        
    public:
        // constructs an FRThread for the specified handle
        // be careful with this. there's no way to check if the handle is valid
        // if it's null it's set to the handle of the current task
        FRThread(const TaskHandle_t handle) :m_handle(nullptr!=handle?handle:xTaskGetCurrentTaskHandle()) {
        }
        // constructs an empty, invalid FRThread instance
        FRThread() :m_handle(nullptr) {
        }
        FRThread(const FRThread& rhs) : m_handle(rhs.m_handle) {
        }
        FRThread(FRThread&& rhs) : m_handle(rhs.m_handle) {
        }
        FRThread& operator=(const FRThread& rhs) {
            m_handle=rhs.m_handle;
            return *this;
        }
        FRThread& operator=(FRThread&& rhs) {
            m_handle=rhs.m_handle;
            return *this;
        }
        virtual ~FRThread() =default;
        // retrieves the handle
        TaskHandle_t handle() const {
            return m_handle;
        }
        // sleeps the calling thread for at least the specified number of milliseconds
        static void sleep(TickType_t milliseconds) {
            vTaskDelay(pdMS_TO_TICKS(milliseconds));
        }
        // retrieves the calling thread
        static FRThread current() {
            return FRThread(xTaskGetCurrentTaskHandle());
        }
        // retrieves the current running thread on the given CPU or core
        static FRThread current(BaseType_t cpu) {
            return FRThread(xTaskGetCurrentTaskHandleForCPU(cpu));
        }
        // retrieves the idle thread
        static FRThread idle() {
            return FRThread(xTaskGetIdleTaskHandle());
        }
        // retrieves the idle thread on the given CPU or core
        static FRThread idle(BaseType_t cpu) {
            return FRThread(xTaskGetIdleTaskHandleForCPU(cpu));
        }
        // creates a thread
        static FRThread create(std::function<void(const void*)> fn,const void* state,UBaseType_t priority=tskIDLE_PRIORITY+1, uint32_t stackWordSize=1024) {
            TaskHandle_t handle = nullptr;
            TaskEntryThunk_t f2p;
            f2p.callingThreadHandle=xTaskGetCurrentTaskHandle();
            f2p.fn = fn;
            f2p.state = state;
            if(pdPASS==xTaskCreate(taskEntryThunk,"FRThread",stackWordSize,&f2p,priority,&handle)) {
                FRThread result(handle);
                ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
                return result;
            }
            return FRThread(nullptr);
        }
        // creates a thread on the specified CPU
        static FRThread createAffinity(std::function<void(const void*)> fn,const void* state,BaseType_t cpu,UBaseType_t priority=tskIDLE_PRIORITY+1, uint32_t stackWordSize=1024) {
            TaskHandle_t handle = nullptr;
            TaskEntryThunk_t f2p;
            f2p.callingThreadHandle=xTaskGetCurrentTaskHandle();
            f2p.fn = fn;
            f2p.state = state;
            if(pdPASS==xTaskCreatePinnedToCore(taskEntryThunk,"FRThread",stackWordSize,&f2p, priority,&handle,cpu)) {
                FRThread result(handle);
                ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
                return result;
            }
            return FRThread(nullptr);
        }
        // creates a dispatcher thread for the thread pool. Use FRThreadPool.createThread() instead
        static FRThread createDispatcher(FRThreadPoolCreationData_t& data,UBaseType_t priority=tskIDLE_PRIORITY+1, uint32_t stackWordSize=1024) {
            TaskHandle_t handle = nullptr;
            DispatcherEntry_t de;
            de.callingThreadHandle=xTaskGetCurrentTaskHandle();
            de.psyncContext=data.psyncContext;
            de.pshutdownThreadHandle=data.ptaskHandle;
            de.pthreadCount=data.pthreadCount;
            if(pdPASS==xTaskCreate(dispatcherEntry,"FRDispatcherThread",stackWordSize,&de,priority,&handle)) {
                FRThread result(handle);
                ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
                return result;
            }
            return FRThread(nullptr);
        }
        // creates a dispatcher thread for the thread pool on the specified CPU or core. Use FRThreadPool.createThreadAffinity() instead
       static FRThread createDispatcherAffinity(FRThreadPoolCreationData_t& data,BaseType_t cpu,UBaseType_t priority=tskIDLE_PRIORITY+1, uint32_t stackWordSize=1024) {
            TaskHandle_t handle = nullptr;
            DispatcherEntry_t de;
            de.callingThreadHandle=xTaskGetCurrentTaskHandle();
            de.psyncContext=data.psyncContext;
            de.pshutdownThreadHandle=data.ptaskHandle;
            de.pthreadCount=data.pthreadCount;
            if(pdPASS==xTaskCreatePinnedToCore(dispatcherEntry,"FRDispatcherThread",stackWordSize,&de, priority,&handle,cpu)) {
                FRThread result(handle);
                ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
                return result;
            }
            return FRThread(nullptr);
        }
        // gets the priority of the the thread
        UBaseType_t priority() const {
            if(nullptr!=m_handle) {
                return uxTaskPriorityGet(m_handle);
            }
            return 0;
        }
        // sets the priority of the thread
        bool priority(UBaseType_t value) {
            if(nullptr!=m_handle) {
                vTaskPrioritySet(m_handle,value);
                return true;
            }
            return false;
        }
        // gets the affinity of the thread
        BaseType_t affinity() const {
            if(nullptr==m_handle) return 0;
            return xTaskGetAffinity(m_handle);
        }
        // starts or resumes the thread
        bool start() {
            if(nullptr!=m_handle) {
                vTaskResume(m_handle);
                return true;
            }
            return false;
        }
        // forcibly aborts the thread
        bool abort() {
            if(nullptr!=m_handle) {
                vTaskDelete(m_handle);
                m_handle=nullptr;
                return true;
            }
            return false;
        }
        // suspends the thread
        bool suspend() {
            if(nullptr!=m_handle) {
                vTaskSuspend(m_handle);
                return true;
            }
            return false;
        }
        // indicates whether or not the thread is running rather than suspended, idle or blocked
        bool isRunning() const {
            if(nullptr!=m_handle) {
               eTaskState state = eTaskGetState(m_handle);
               return (state==eTaskState::eRunning);
            }
            return false;
        }

};
// Provides a thread pool for enqueuing and executing long running tasks in the background
class FRThreadPool {
    TaskHandle_t m_shutDownTaskHandle;
    std::atomic<size_t> m_threadCount;
    FRSynchronizationContext m_dispatcher;
    
public:
    // constructs a thread pool. The queueSize indicates how many work items can be waiting on available threads before queueUserWorkItem() starts to block
    FRThreadPool(size_t queueSize=32) : m_threadCount(0), m_dispatcher(queueSize)  {}
    FRThreadPool(const FRThreadPool& rhs) =delete;
    FRThreadPool& operator=(const FRThreadPool& rhs)=delete;
    FRThreadPool(FRThreadPool&& rhs)=delete;
    FRThreadPool& operator=(FRThreadPool&& rhs)=delete;
    virtual ~FRThreadPool() {
       m_shutDownTaskHandle = xTaskGetCurrentTaskHandle();
        while(m_threadCount>0) {
            m_dispatcher.postQuit();
            ulTaskNotifyTake(pdFALSE,portMAX_DELAY);
        }
        m_shutDownTaskHandle=nullptr;
    }
    // sends the shutdown signal to all pooled threads, leading them to exit after the current operation.
    // if abort is true, then each thread in the pool is forcibly aborted
    // this method does not block
    bool shutdown(bool abort=false) {
        size_t s = m_threadCount;
        if(!abort) {
            for(size_t i = 0;i<s;++i)
                m_dispatcher.postQuit();
        } else {
            for(size_t i = 0;i<s;++i) {
                m_dispatcher.post([](const void*){vTaskDelete(NULL); });
            }
            m_threadCount=0;
        }
        return true;
    }
    // enqueues a work item for execution on one of the pooled threads
    // work will commence once a thread becomes available.
    // this function will not block unless there are too many backlogged
    // items waiting for an available pool thread.
    bool queueUserWorkItem(std::function<void(void*)> fn,void* state) {
        return m_dispatcher.post(fn,state);
    }
    // creates a thread for the pool
    FRThread createThread(UBaseType_t priority = tskIDLE_PRIORITY+1,uint32_t stackWordSize=1000) {
        FRThreadPoolCreationData_t data;
        data.psyncContext=&m_dispatcher;
        data.ptaskHandle=&m_shutDownTaskHandle;
        data.pthreadCount=&m_threadCount;
        return FRThread::createDispatcher(data,priority,stackWordSize);
    }
    // creates a thread for the pool on the specified CPU
    FRThread createThreadAffinity(BaseType_t cpu, UBaseType_t priority = tskIDLE_PRIORITY+1,uint32_t stackWordSize=1000) {
        FRThreadPoolCreationData_t data;
        data.psyncContext=&m_dispatcher;
        data.ptaskHandle=&m_shutDownTaskHandle;
        data.pthreadCount=&m_threadCount;
        return FRThread::createDispatcherAffinity(data,cpu,priority,stackWordSize);
    }
};
#endif