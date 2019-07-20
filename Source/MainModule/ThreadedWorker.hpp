/*
 * Threading support for MulticopterSim
 *
 * Copyright (C) 2019 Simon D. Levy
 *
 * MIT License
 */

#pragma once

#include "Core.h"
#include "Runnable.h"

#include <stdarg.h>

class FThreadedWorker : public FRunnable {

    private:

        FRunnableThread * _thread = NULL;

        bool _running = false;

        // Supports debugging on main thread
        static const uint16_t MAXMSG = 1000;

        // Start-time offset so timing begins at zero
        double _startTime = 0;

        // For FPS reporting
        uint32_t _count;

    protected:

        // Supports debugging on main thread
        char _message[MAXMSG] = {0};

        // Implemented differently by each subclass
        virtual void performTask(double currentTime) = 0;

    public:

        FThreadedWorker(void)
        {
            _thread = FRunnableThread::Create(this, TEXT("FThreadedWorker"), 0, TPri_BelowNormal); 

            *_message = 0;

            _startTime = FPlatformTime::Seconds();

            _count = 0;
        }


        ~FThreadedWorker()
        {
            delete _thread;
        }


        // Supports debugging on main thread
        void dbgprintf(const char * fmt, ...)
        {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(_message, MAXMSG, fmt, ap);
            va_end(ap);
        }

        const char * getMessage(void)
        {
            return (const char *)_message;
        }

        uint32_t getCount(void)
        {
            return _count;
        }

        static FThreadedWorker * stopThreadedWorker(FThreadedWorker * worker)
        {
            if (worker) {
                worker->Stop();
                delete worker;
            }

            return (FThreadedWorker *)NULL;
        }

        // FRunnable interface.

        virtual bool Init() override
        {
            _running = false;

			return FRunnable::Init();
        }

        virtual uint32_t Run() override
        {
            // Initial wait before starting
            FPlatformProcess::Sleep(0.5);

            _running = true;

            while (_running) {

                // Get a high-fidelity current time value from the OS
                double currentTime = FPlatformTime::Seconds() - _startTime;

                // Pass current time to task implementation
                performTask(currentTime);

                // Increment count for FPS reporting
                _count++;

                // Wait a bit to allow other threads to run
                FPlatformProcess::Sleep(.0001); 
            }

			return 0;
        }

        virtual void Stop() override
        {
            _running = false;

            // Final wait after stopping
            FPlatformProcess::Sleep(0.03);

			FRunnable::Stop();
        }

}; // class FThreadedWorker