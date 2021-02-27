#include <FRThreadPack.h>
// use this to synchronize calls by executing functors on the target thread
// this one runs on our main thread, so anything posted to it gets run there
FRSynchronizationContext g_mainSync;

// just something we can increment
// we only touch this from the main thread
// so it doesn't need to be synchronized
unsigned long long g_count;

void setup()
{
  g_count = 0;
  Serial.begin(115200);

  // check the initialization of our synchronization context
  if (!g_mainSync.handle())
  {
    Serial.println("Error initializing synchronization context");
    while (true)
      ; // halt
  }

  // now, we can send() or post() code to g_mainSync to get it executed
  // on the main thread. use this to synchronize.

  // create a thread pool
  // note that this thread pool
  // will go out of scope and all
  // created threads will be exited
  // once setup() ends
  FRThreadPool pool;
  // create three threads for the pool
  // all of these threads are now waiting on incoming items.
  // once an item becomes available, one of the threads will
  // dispatch it. When it's complete, it will return to the
  // listening state. You do not use this thread pool the way
  // you use .NET's thread pool. .NET's thread pool
  // has lots of reserve threads created by the system.
  // This threadpool has no threads unless you create them.
  pool.createThread(); 
  pool.createThread();
  pool.createThread();
  
  // now queue up 4 work items. The first 3 will start executing immediately
  // the 4th one will start executing once one of the others exits.
  // this is because we have 3 threads available.
  pool.queueUserWorkItem([](void*state){
    delay(3000);
    Serial.println("Work item 1");
  },nullptr);
  pool.queueUserWorkItem([](void*state){
    delay(2000);
    Serial.println("Work item 2");
  },nullptr);
  pool.queueUserWorkItem([](void*state){
    delay(1000);
    Serial.println("Work item 3");
  },nullptr);
  pool.queueUserWorkItem([](void*state){
    Serial.println("Work item 4");
  },nullptr);
  
  // create a thread
  FRThread thread1 = FRThread::create([](const void *state) {
    // This thread just posts Hello World 1! and a count
    // to the target thread over and over again, delaying
    // by 3/4 of a second each time

    // infinite loop 
    while (true)
    {
      // use post() to execute code on the target thread
      // - does not block here
      g_mainSync.post([](void *state) {
        // BEGIN EXECUTE ON TARGET THREAD
        Serial.printf("Hello world 1! - Count: %llu\r\n", g_count);
        // normally we couldn't access g_count
        // from a different thread safely
        // but this always runs on the target
        // thread so we're okay since g_count
        // never gets touched by any other
        // thread
        ++g_count;
        // END EXECUTE ON TARGET THREAD
      });
      // EXECUTES ON THIS THREAD:
      delay(750);
    }
  },nullptr,4);
  // if thread1 handle is null, create failed.
  if(!thread1.handle()) {
    Serial.println("Could not create thread 1");
    while(true); // halt
  }
  if(!thread1.start()) {
    Serial.println("Failed to start thread 1");
    while(true); // halt
  }
 
  // create a thread 
  FRThread thread2 = FRThread::create([](const void *state) {
    // This thread just posts Hello World 2! and a count
    // to the target thread over and over again, delaying
    // by 3/4 of a second each time

    // infinite loop 
    while (true)
    {
      // use send() to execute code on the target thread
      // - send() blocks until the sent code returns
      g_mainSync.send([](void *state) {
        // BEGIN EXECUTE ON TARGET THREAD
        Serial.printf("Hello world 2! - Count: %llu\r\n", g_count);
        // normally we couldn't access g_count
        // from a different thread safely
        ++g_count;
        // END EXECUTE ON TARGET THREAD
      });
      // EXECUTES ON THIS THREAD:
      delay(1000);
    }
  },nullptr,1);
  // make sure the createAffinity() call succeeded:
  if(!thread2.handle()) {
    Serial.println("Could not create thread 2");
    while(true); // halt
  }
  if(!thread2.start()) {
    Serial.println("Failed to start thread 2");
    while(true); // halt
  }
 
  // thread1 and thread2 never die. you'd have to call abort() to get rid of them since 
  // they each run an infinite loop. their lifetime is not tied to FRThread instances

  // the thread pool exits here, waiting for pool threads to complete
  // so this can take some time. meanwhile the other two threads,
  // thread1 and thread2 are currently working in the background
  // but messages won't be displayed until the pool exits,
  // because the two threads are dispatching their results
  // onto this thread, which is asleep until pool finishes
  // exiting.

  // in production code you wouldn't want the above to suspend
  // the current thread too long because it will create a backlog
  // in g_mainSync. One thing you could do is call pool.shutdown()
  // earlier in your code to begin the pool shutdown process.
  // that way by the time FRThreadPool goes out of scope your
  // threads are already being destroyed
}

void loop()
{

  // this simply dispatches calls made by send() or post()
  // by executing them here. Note that long running methods
  // or a backlogged queue can cause this to block for a
  // significant amount of time. Try to avoid sending/posting long
  // running calls to the synchronization contexts themselves -
  // that's what threads are for anyway.
  // THIS IS THE TARGET THREAD FOR SEND AND POST FROM ABOVE:
  if (!g_mainSync.update())
  {
    Serial.println("Could not update synchronization context");
  }
}
