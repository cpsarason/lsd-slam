

#include "App/InputThread.h"
#include "App.h"

namespace lsd_slam {


  InputThread::InputThread(  std::shared_ptr<lsd_slam::SlamSystem> &sys,
                              std::shared_ptr<libvideoio::ImageSource> &src,
                              std::shared_ptr<libvideoio::Undistorter> &und )
    : system( sys ), dataSource( src ), undistorter( und ),
    inputDone( false ),
    inputReady(),
    output( nullptr )
    {
      ;
    }

    void InputThread::setIOOutputWrapper( const std::shared_ptr<lsd_slam::OutputIOWrapper> &out )
    {
      output = out;
    }

    void InputThread::operator()() {
      // get HZ
      float fps = dataSource->fps();
      long int dt_us = (fps > 0) ? (1e6/fps) : 0;
      long int dt_wiggle = 0;

      inputReady.notify();

      startAll.wait();

      int numFrames = dataSource->numFrames();
      LOG_IF( INFO, numFrames > 0 ) << "Running for " << numFrames << " frames at " << fps << " fps";

      cv::Mat image = cv::Mat(system->conf().slamImage.cvSize(), CV_8U);
      int runningIdx=0;
      float fakeTimeStamp = 0;

      for(unsigned int i = 0; (numFrames < 0) || (i < (unsigned int)numFrames); ++i)
      {
        if(inputDone.getValue()) break;

        std::chrono::time_point<std::chrono::steady_clock> start(std::chrono::steady_clock::now());

        if( dataSource->grab() ) {
          if( dataSource->getImage( image ) >= 0 ) {
            CHECK(image.type() == CV_8UC1);

            cv::Mat imageUndist;
            undistorter->undistort(image, imageUndist);

            system->trackFrame( new Frame( runningIdx, system->conf(), fakeTimeStamp, imageUndist.data ), fps == 0 );

            runningIdx++;
            fakeTimeStamp += (fps > 0) ? (1.0/fps) : 0.03;

            if( output ) {
              output->updateFrameNumber( runningIdx );
              output->updateLiveImage( imageUndist );
            }

          }

          if(fullResetRequested)
          {
            LOG(WARNING) << "FULL RESET!";
            system.reset( system->fullReset() );

            fullResetRequested = false;
            runningIdx = 0;
          }

        } else {
          LOG(INFO) << "Bad read, still running...";
          if( system->conf().stopOnFailedRead ) break;
        }

        if( dt_us > 0 ) std::this_thread::sleep_until( start + std::chrono::microseconds( dt_us + dt_wiggle ) );
      }

      LOG(INFO) << "Have processed all input frames.";
      inputDone.assignValue(true);
    }

  }
