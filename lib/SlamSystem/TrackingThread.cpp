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

#include <boost/thread/shared_lock_guard.hpp>

#include "TrackingThread.h"

#include "SlamSystem.h"

// #include "DataStructures/Frame.h"
#include "Tracking/SE3Tracker.h"
// #include "Tracking/Sim3Tracker.h"
// #include "DepthEstimation/DepthMap.h"
#include "Tracking/TrackingReference.h"
// #include "util/globalFuncs.h"
#include "GlobalMapping/KeyFrameGraph.h"
#include "GlobalMapping/TrackableKeyFrameSearch.h"
// #include "GlobalMapping/g2oTypeSim3Sophus.h"
// #include "IOWrapper/ImageDisplay.h"
// #include "IOWrapper/Output3DWrapper.h"
// #include <g2o/core/robust_kernel_impl.h>
// #include "DataStructures/FrameMemory.h"
// #include "deque

#include "SlamSystem/MappingThread.h"

// for mkdir
#include <sys/types.h>
#include <sys/stat.h>

#include <g3log/g3log.hpp>

#ifdef ANDROID
#include <android/log.h>
#endif

#include "opencv2/opencv.hpp"

using namespace lsd_slam;



TrackingThread::TrackingThread( SlamSystem &system )
: _system( system ),
//	_system.currentKeyFrame( system.currentKeyFrame ),
	_tracker( new SE3Tracker( system.conf().slamImage ) ),
	_trackingReference( new TrackingReference() ),
	_trackingIsGood( true )
{


	// this->width = w;
	// this->height = h;
	// this->K = K;
	// trackingIsGood = true;

	// keyFrameGraph = new KeyFrameGraph();

	// createNewKeyFrame = false;

	// map =  new DepthMap( conf );

	// newConstraintAdded = false;
	//haveUnmergedOptimizationOffset = false;


	// Do not use more than 4 levels for odometry tracking
	for (int level = 4; level < PYRAMID_LEVELS; ++level)
		_tracker->settings.maxItsPerLvl[level] = 0;

	// trackingReference = new TrackingReference();
	//mappingTrackingReference = new TrackingReference();


	// if(SLAMEnabled)
	// {
	// 	trackableKeyFrameSearch = new TrackableKeyFrameSearch(keyFrameGraph,conf);
	// 	constraintTracker = new Sim3Tracker( _conf.slamImage );
	// 	constraintSE3Tracker = new SE3Tracker( _conf.slamImage );
	// 	newKFTrackingReference = new TrackingReference();
	// 	candidateTrackingReference = new TrackingReference();
	// }
	// else
	// {
	// 	constraintSE3Tracker = 0;
	// 	trackableKeyFrameSearch = 0;
	// 	constraintTracker = 0;
	// 	newKFTrackingReference = 0;
	// 	candidateTrackingReference = 0;
	// }


	// outputWrapper = 0;

	// keepRunning = true;
	// depthMapScreenshotFlag = false;
	lastTrackingClosenessScore = 0;

	timeLastUpdate.start();
}


TrackingThread::~TrackingThread()
{

	delete _trackingReference;
	delete _tracker;
}


void TrackingThread::trackFrame(std::shared_ptr<Frame> newFrame, bool blockUntilMapped )
{
	// Create new frame
	// std::shared_ptr<Frame> trackingNewFrame(new Frame(frameID, _conf, timestamp, image));

	//LOG(INFO) << "In trackFrame";

	if(!_trackingIsGood)
	{
		// Prod mapping to check the relocalizer
		_system.mapThread->relocalizer.updateCurrentFrame(newFrame);
		//_system.mapThread->doIteration();

//		unmappedTrackedFrames.notifyAll();

		// {
		// 	std::lock_guard< std::mutex > lock( unmappedTrackedFramesMutex );
		// 	unmappedTrackedFramesSignal.notify_one();
		// }
		return;
	}

	// Are the following two calls atomic enough or should I lock
	// before the next two lines?
	//bool newKeyFramePending = _system.mapThread->newKeyFramePending();	// pre-save here, to make decision afterwards.
	Frame::SharedPtr keyframe( _system.currentKeyFrame().get() );

	if(_trackingReference->frameID != keyframe->id() || keyframe->depthHasBeenUpdatedFlag )
	{
		LOG(DEBUG) << "Importing new tracking reference from frame " << keyframe->id();
		_trackingReference->importFrame( keyframe );
		keyframe->depthHasBeenUpdatedFlag = false;
		_trackingReferenceFrameSharedPT = keyframe;
	}

	FramePoseStruct &trackingReferencePose( *_trackingReference->keyframe->pose);

	// DO TRACKING & Show tracking result.
	LOG_IF(DEBUG, enablePrintDebugInfo && printThreadingInfo) << "TRACKING frame " << newFrame->id() << " onto ref. " << _trackingReference->frameID;


	SE3 frameToReference_initialEstimate;
	{
		boost::shared_lock_guard<boost::shared_mutex> lock( _system.poseConsistencyMutex );
		frameToReference_initialEstimate = se3FromSim3( trackingReferencePose.getCamToWorld().inverse() * _system.allFramePoses.const_ref().back()->getCamToWorld());
	}


	Timer timer;

	SE3 newRefToFrame_poseUpdate = _tracker->trackFrame(
																	_trackingReference,
																	newFrame.get(),
																	frameToReference_initialEstimate);

	perf.update( timer );

	tracking_lastResidual = _tracker->lastResidual;
	tracking_lastUsage = _tracker->pointUsage;
	//tracking_lastGoodPerBad = _tracker->lastGoodCount / (_tracker->lastGoodCount + _tracker->lastBadCount);
	//tracking_lastGoodPerTotal = _tracker->lastGoodCount / (newFrame->width(SE3TRACKING_MIN_LEVEL)*newFrame->height(SE3TRACKING_MIN_LEVEL));


	if(manualTrackingLossIndicated || _tracker->diverged ||
		(_system.keyframesAll.const_ref().size() > INITIALIZATION_PHASE_COUNT &&
		 !_tracker->trackingWasGood))
	{
		LOGF(WARNING, "TRACKING LOST for frame %d (%1.2f%% good Points, which is %1.2f%% of available points; %s tracking; tracker has %s)!\n",
				newFrame->id(),
				100*_tracker->_pctGoodPerTotal,
				100*_tracker->_pctGoodPerGoodBad,
				_tracker->trackingWasGood ? "GOOD" : "BAD",
				_tracker->diverged ? "DIVERGED" : "NOT DIVERGED");

		_trackingReference->invalidate();

		setTrackingIsBad();
		//nextRelocIdx = -1;  // What does this do?

		// Kick over the mapping thread
		//_system.mapThread->doIteration();
		// unmappedTrackedFrames.notifyAll();

		// unmappedTrackedFramesMutex.lock();
		// unmappedTrackedFramesSignal.notify_one();
		// unmappedTrackedFramesMutex.unlock();

		manualTrackingLossIndicated = false;
		return;
	}



	// if(plotTracking)
	// {
	// 	Eigen::Matrix<float, 20, 1> data;
	// 	data.setZero();
	// 	data[0] = _tracker->lastResidual;
	//
	// 	data[3] = _tracker->lastGoodCount / (tracker->lastGoodCount + _tracker->lastBadCount);
	// 	data[4] = 4*tracker->lastGoodCount / (float)_conf.slamImage.area();
	// 	data[5] = _tracker->pointUsage;
	//
	// 	data[6] = _tracker->affineEstimation_a;
	// 	data[7] = _tracker->affineEstimation_b;
	// 	outputWrapper->publishDebugInfo(data);
	// }


		_system.storePose(newFrame);

	//Sim3 lastTrackedCamToWorld = mostCurrentTrackedFrame->getCamToWorld();
//  mostCurrentTrackedFrame->TrackingParent->getCamToWorld() * sim3FromSE3(mostCurrentTrackedFrame->thisToParent_SE3TrackingResult, 1.0);

	LOG_IF( DEBUG,  enablePrintDebugInfo && printThreadingInfo ) << "Publishing tracked frame";
	_system.publishTrackedFrame(newFrame);
	_system.publishPose(newFrame->getCamToWorld().cast<float>());

	// Keyframe selection
	// latestTrackedFrame = trackingNewFrame;
	//if (!my_createNewKeyframe && _map.currentKeyFrame()->numMappedOnThisTotal > MIN_NUM_MAPPED)
	LOG(INFO) << "While tracking " << newFrame->id() << " the keyframe is " << _system.currentKeyFrame().const_ref()->id();
	LOG_IF( INFO, printThreadingInfo ) << _system.currentKeyFrame().const_ref()->numMappedOnThisTotal << " frames mapped on to keyframe " << _system.currentKeyFrame().const_ref()->id() << ", considering " << newFrame->id() << " as new keyframe.";

	LOG_IF( DEBUG, printThreadingInfo ) << "Push unmapped tracked frame.";
	_system.mapThread->mapTrackedFrame( newFrame, blockUntilMapped );


	//bool nominateNewKeyframe = false;
//	if(!newKeyFramePending && _system.currentKeyFrame().const_ref()->numMappedOnThisTotal > MIN_NUM_MAPPED)

	if( _system.currentKeyFrame().const_ref()->numMappedOnThisTotal > MIN_NUM_MAPPED)
	{
		Sophus::Vector3d dist = newRefToFrame_poseUpdate.translation() * _system.currentKeyFrame().const_ref()->meanIdepth;
		float minVal = fmin(0.2f + _system.keyframesAll.const_ref().size() * 0.8f / INITIALIZATION_PHASE_COUNT,1.0f);

		if(_system.keyframesAll.const_ref().size() < INITIALIZATION_PHASE_COUNT)	minVal *= 0.7;

		auto d2 = dist.dot(dist);
		lastTrackingClosenessScore = _system.trackableKeyFrameSearch()->getRefFrameScore(d2, _tracker->pointUsage);

		if (lastTrackingClosenessScore > minVal)
		{
			LOG(INFO) << "Telling mapping thread to make " << newFrame->id() << " the new keyframe.";
			//_system.mapThread->createNewKeyFrame( newFrame );
			//nominateNewKeyframe = true;
			// createNewKeyFrame = true;
			LOGF_IF( INFO, printKeyframeSelectionInfo,
							"SELECT KEYFRAME %d on %d! f(dist %.3f, usage %.3f) = %.3f > %.3f",
							newFrame->id(),newFrame->trackingParent()->id(),
							d2,
							_tracker->pointUsage,
							lastTrackingClosenessScore,
							minVal );

			_system.changeKeyframe(newFrame, false, true, 1.0f);

		}
		else
		{
			LOGF_IF( INFO, printKeyframeSelectionInfo,
							"SKIPPD KEYFRAME %d on %d! f(dist %.3f, usage %.3f) = %.3f <= %.3f",
							newFrame->id(),newFrame->trackingParent()->id(),
							d2,
							_tracker->pointUsage,
							lastTrackingClosenessScore,
							minVal );
		}
	}

	// TODO:  Change nominateNewKeyframe to be a poke into _slamSystem.

	// unmappedTrackedFrames.notifyAll();
		// unmappedTrackedFramesSignal.notify_one();
	// }

	// If blocking is requested...
	// if(blockUntilMapped && trackingIsGood() ){
	// 	while( _system.mapThread->unmappedTrackedFrames.size() > 0 ) {
	// 		_system.mapThread->trackedFramesMapped.wait( );
	// 	}
	// }

	LOG_IF( DEBUG, printThreadingInfo ) << "Exiting trackFrame";

}



// n.b. this function will be called from the mapping thread.  Ensure
// locking is in place.
void TrackingThread::takeRelocalizeResult( const RelocalizerResult &result  )
{

	// Frame* keyframe;
	// int succFrameID;
	// SE3 succFrameToKF_init;
	// std::shared_ptr<Frame> succFrame;
	//
	// relocalizer.stop();
	// relocalizer.getResult(keyframe, succFrame, succFrameID, succFrameToKF_init);
	// assert(keyframe != 0);

	Frame::SharedPtr keyframe(_system.currentKeyFrame().const_ref() );
	_trackingReference->importFrame( keyframe );
	_trackingReferenceFrameSharedPT = keyframe;

	_tracker->trackFrame(
			_trackingReference,
			result.successfulFrame.get(),
			result.successfulFrameToKeyframe );

	if(!_tracker->trackingWasGood || _tracker->lastGoodCount() / (_tracker->lastGoodCount()) < 1-0.75f*(1-MIN_GOODPERGOODBAD_PIXEL))
	{
		LOG_IF(DEBUG, enablePrintDebugInfo && printRelocalizationInfo) << "RELOCALIZATION FAILED BADLY! discarding result.";
		_trackingReference->invalidate();
	}
	else
	{
		_system.storePose(result.successfulFrame );

		_system.mapThread->mapTrackedFrame( result.successfulFrame, true );

		// {
		// 	std::lock_guard<std::mutex> lock( currentKeyFrameMutex );
			// createNewKeyFrame = false;
			setTrackingIsGood();
		//}
	}

}
