/**
* This file is part of LSD-SLAM.
*
* Copyright 2013 Jakob Engel <engelj at in dot tum dot de> (Technical University of Munich)
* For more information see <http://vision.in.tum.de/lsdslam>
*
* LSD-SLAM is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* LSD-SLAM is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with LSD-SLAM. If not, see <http://www.gnu.org/licenses/>.
*/

#include <zed/Camera.hpp>

#include "LiveSLAMWrapper.h"

#include <boost/thread.hpp>
#include "util/settings.h"
#include "util/Parse.h"
#include "util/globalFuncs.h"
#include "util/ThreadMutexObject.h"
#include "IOWrapper/Pangolin/PangolinOutput3DWrapper.h"
#include "SlamSystem.h"

#include <sstream>
#include <fstream>
#include <dirent.h>
#include <algorithm>

#include <glog/logging.h>

#include <tclap/CmdLine.h>

#include "util/Undistorter.h"

#include "opencv2/opencv.hpp"

#include "GUI.h"

const sl::zed::ZEDResolution_mode zedResolution = sl::zed::HD1080;

// 1080 is not divisible by 16
const cv::Size originalSize( 1920, 1080 );
const cv::Size cropSize( 1920, 1056 );
const cv::Size scaledSize( cropSize.width / 2, cropSize.height / 2 );
const cv::Size slamSize( scaledSize );


ThreadMutexObject<bool> lsdDone(false);
GUI gui( scaledSize.width * 1.0 / scaledSize.height );

int numFrames = 0;

sl::zed::Camera *camera = NULL;
enum { NO_STEREO, STEREO_ZED } doStereo = NO_STEREO;

using namespace lsd_slam;

void run(SlamSystem * system, Output3DWrapper* outputWrapper, Sophus::Matrix3f K)
{
    // get HZ
    double hz = camera->getCurrentFPS();
    if( hz < 0 ) {
      printf("Unable to get FPS from input, using 30\n");
      hz = 30;
    }

    useconds_t dt_msec = (1.0/hz) * 1e3;

    int runningIdx = 0;
    float fakeTimeStamp = 0;

    for(unsigned int i = 0; (numFrames < 0) || (i < numFrames); i++, runningIdx++)
    {
        if(lsdDone.getValue())
            break;

        // printf("Loop %d\n", runningIDX );

        gui.updateFrameNumber( i );

        if(fullResetRequested)
        {
            printf("FULL RESET!\n");
            delete system;

            system = new SlamSystem(slamSize.width, slamSize.height, K, doSlam);
            system->setVisualization(outputWrapper);

            fullResetRequested = false;
            runningIdx = 0;
        }

        if( camera->grab( sl::zed::SENSING_MODE::RAW, false, false ) ) {
          LOG(ERROR) << "Error reading data from camera";
          continue;
        }

        sl::zed::Mat left = camera->retrieveImage(sl::zed::SIDE::LEFT);

        cv::Mat imageROI;
        imageROI = cv::Mat( sl::zed::slMat2cvMat(left), cv::Rect( cv::Point(0,0), cropSize) );

        // Convert to greyscale
        cv::Mat imageGray( cropSize, CV_8UC1 );
        cv::cvtColor( imageROI, imageGray, cv::COLOR_BGRA2GRAY );

        cv::Mat imageScaled;
        if( scaledSize != cropSize ) {
          // Shrink (for now)
          cv::resize( imageGray, imageScaled, scaledSize );
        } else {
          imageScaled = imageGray;
        }

        CHECK( imageScaled.type() == CV_8U );
        CHECK( (imageScaled.rows == slamSize.height) && (imageScaled.cols == slamSize.width) );

        gui.updateLiveImage( imageScaled.data );

        if( doStereo == STEREO_ZED ) {

          // Fetch depth map as well
          sl::zed::Mat depth = camera->retrieveMeasure( sl::zed::DEPTH );
          cv::Mat depthCropped( sl::zed::slMat2cvMat(depth), cv::Rect( cv::Point(0,0), cropSize) );
          cv::Mat depthScaled( scaledSize, CV_32FC1 );
          cv::resize( depthCropped, depthScaled, scaledSize );

          CHECK( depthScaled.type() == CV_32FC1 );
          CHECK( (imageScaled.rows == depthScaled.rows) && (imageScaled.cols == depthScaled.cols) );

          if(runningIdx == 0) {
            system->gtDepthInit(imageScaled.data, (float *)depthScaled.data, fakeTimeStamp, runningIdx);
          } else {
            system->trackStereoFrame(imageScaled.data, (float *)depthScaled.data, runningIdx, hz == 0, fakeTimeStamp);
          }

        } else {
          if(runningIdx == 0)
          {
            system->randomInit(imageScaled.data, fakeTimeStamp, runningIdx);
          }
          else
          {
            system->trackFrame(imageScaled.data, runningIdx, hz == 0, fakeTimeStamp);
          }
        }

        gui.pose.assignValue(system->getCurrentPoseEstimateScale());

        fakeTimeStamp+=0.03;


        boost::this_thread::sleep_for(boost::chrono::milliseconds(dt_msec));
    }

    lsdDone.assignValue(true);
}

int main( int argc, char** argv )
{
  // Initialize Google logging
  google::InitGoogleLogging( argv[0] );
  FLAGS_logtostderr = true;
  FLAGS_minloglevel = 0;

  try {
    TCLAP::CmdLine cmd("LSD_Zed", ' ', "0.1");

    TCLAP::ValueArg<std::string> svoFileArg("i","input","Name of SVO file to read",false,"","SVO filename", cmd);
    TCLAP::SwitchArg stereoSwitch("","stereo","Use stereo data", cmd, false);

    cmd.parse(argc, argv );

  	if( svoFileArg.isSet() > 0)
  	{
      LOG(INFO) << "Loading SVO file " << svoFileArg.getValue();
      camera = new sl::zed::Camera( svoFileArg.getValue() );
      numFrames = camera->getSVONumberOfFrames();
  	} else {
      LOG(INFO) << "Using live Zed data";
      camera = new sl::zed::Camera( zedResolution );
      numFrames = -1;
    }

    if( stereoSwitch.getValue() ) {
      LOG(INFO) << "Using stereo data from Stereolabs libraries";
      doStereo = STEREO_ZED;
    }

  } catch (TCLAP::ArgException &e)  // catch any exceptions
	{
    LOG(FATAL) << "error: " << e.error() << " for arg " << e.argId();
  }

  // get camera calibration in form of an undistorter object.
	// if no undistortion is required, the undistorter will just pass images through.
	// std::string calibFile;
	//Undistorter* undistorter = 0;

  sl::zed::MODE zedMode = sl::zed::MODE::NONE;
  if( doStereo == STEREO_ZED ) zedMode = sl::zed::MODE::QUALITY;

  const int whichGpu = -1;
  const bool verboseInit = true;
  sl::zed::ERRCODE err = camera->init( zedMode, whichGpu, verboseInit );
  if (err != sl::zed::SUCCESS) {
    LOG(ERROR) << "Unable to init the zed: " << errcode2str(err);
    delete camera;
    exit(-1);
  }

  sl::zed::StereoParameters *params = camera->getParameters();

  float xscale = slamSize.width * 1.0f / originalSize.width;
  float yscale = slamSize.height * 1.0f / originalSize.height;

  float fx = params->LeftCam.fx * xscale;
	float fy = params->LeftCam.fy * yscale;
	float cx = params->LeftCam.cx * xscale;
	float cy = params->LeftCam.cy * yscale;

  LOG(INFO) << "From Zed:  fx = " << params->LeftCam.fx << "; fy = " << params->LeftCam.fy << "; cx = " << params->LeftCam.cx << "; cy = " << params->LeftCam.cy;
  LOG(INFO) << "Scaled:    fx = " << fx << "; fy = " << fy << "; cx = " << cx << "; cy = " << cy;


	Sophus::Matrix3f K;
	K << fx, 0.0, cx, 0.0, fy, cy, 0.0, 0.0, 1.0;
	Intrinsics::getInstance(fx, fy, cx, cy);

  {
    sl::zed::resolution resolution = camera->getImageSize();

    // Intermediate sizes are set with const Size global (at top of file)
    // Just a quick sanity check that they're appropriate for this
    // camera resolution
    assert( resolution.width >= cropSize.width );
    assert( resolution.height >= cropSize.height );

    // Set up this singletons
  	Resolution::getInstance( slamSize.width, slamSize.height );
  }


	gui.initImages();
	Output3DWrapper* outputWrapper = new PangolinOutput3DWrapper( slamSize.width, slamSize.height, gui);

	// make slam system
	SlamSystem * system = new SlamSystem( slamSize.width, slamSize.height, K, doSlam);
	system->setVisualization(outputWrapper);

  printf("Launching LSD thread\n");
	boost::thread lsdThread(run, system, outputWrapper, K);

	while(!pangolin::ShouldQuit())
	{
	    if(lsdDone.getValue() && !system->finalized)
	    {
	        system->finalize();
	    }

	    gui.preCall();

	    gui.drawKeyframes();

	    gui.drawFrustum();

	    gui.drawImages();

	    gui.postCall();
	}

	lsdDone.assignValue(true);

	lsdThread.join();

  if( camera ) delete camera;

	delete system;
	delete outputWrapper;
	return 0;
}