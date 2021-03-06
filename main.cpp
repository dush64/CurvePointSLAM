#include <stdlib.h>
#include <stdio.h>
#include <cv.h>
#include <cvaux.h>
#include <highgui.h>
#include <sys/time.h>
#include <math.h>
#include "common.h"
#include "kalmanClass.h"
#include "capture.h"
#include "featureDetector.h"
#include "displayClass.h"
#include "dataAssocClass.h"
#include "curveFittingClass.h"
#include "pointFeaturesClass.h"
#include <iostream>
#include <fstream>

//#define USE_POINTS              //Whether or not point features should be used

#ifdef QUAD
        #define LEFT_IMAGE_LAG 0
#else
    #ifdef MEADOWBROOK
        #define LEFT_IMAGE_LAG 0
    #else
        #ifdef CRYSTAL_LAKE
            #define LEFT_IMAGE_LAG 2
        #else
                #define LEFT_IMAGE_LAG  2
        #endif
    #endif
#endif

#define ERROR_THRESHOLD 50000.0
//#define ERROR_THRESHOLD 23000.0

#define FRAME_INTERVAL  1
//#define FRAMES_TO_SKIP  3101
#define FRAMES_TO_SKIP  300
#define NUM_FRAMES_DA_RESET     2       //Number of frames without a valid curve measurement after which 
//we reset the data association and start with new curves


#define CURVE_MIN_OBS   0.5     //Min amount of a curve that needs to be observed before it is introduced

using namespace std;

timeval t_start, t_stop;
timeval start, stop;
float elapsedTime;

CvPoint last_pts[2];
CvPoint current_pts[2];

unsigned int binom[50][50];
        
int main(void)
{
        int cam_index[2];
        cam_index[LEFT] = 1;
        cam_index[RIGHT] = 2;

        //Define classes
        CaptureDevice *camera = new CaptureDevice[NUM_CAMERAS];
	DisplayClass *display_GUI = new DisplayClass;
        KalmanFilter *EKF = new KalmanFilter;
        CurveFittingClass * curveFitter = new CurveFittingClass;
        DataAssocClass * curveMatcher = new DataAssocClass[2];
	FeatureDetector *features = new FeatureDetector[NUM_CAMERAS];
        PointFeaturesClass *pointFeatures = new PointFeaturesClass;

        //Define image data
	IplImage * image[NUM_CAMERAS];
	IplImage * image_raw[NUM_CAMERAS];
	IplImage * last_image_raw[NUM_CAMERAS];
	IplImage * seg[NUM_CAMERAS];
	IplImage * image_color[NUM_CAMERAS];
	IplImage * last_image_color[NUM_CAMERAS];
	IplImage * last_image[NUM_CAMERAS];
	IplImage * last_image_track[NUM_CAMERAS];
        
        //Stereo rectification parameters
        CvSize imageSize = cvSize(PIC_WIDTH,PIC_HEIGHT);
        CvMat * mx[2], * my[2];
        mx[0] = cvCreateMat( imageSize.height,
            imageSize.width, CV_32F );
        my[0] = cvCreateMat( imageSize.height,
            imageSize.width, CV_32F );
        mx[1] = cvCreateMat( imageSize.height,
            imageSize.width, CV_32F );
        my[1] = cvCreateMat( imageSize.height,
            imageSize.width, CV_32F );
        CvMat* disp = cvCreateMat( imageSize.height,
            imageSize.width, CV_16S );
        CvRect roi[2];
        CvMat *M1 = cvCreateMat(3, 3, CV_64F);
        CvMat *M2 = cvCreateMat(3, 3, CV_64F);
        CvMat *D1 = cvCreateMat(1, 5, CV_64F);
        CvMat *D2 = cvCreateMat(1, 5, CV_64F);
        CvMat *R = cvCreateMat(3, 3, CV_64F);
        CvMat *T = cvCreateMat(3, 1, CV_64F);
        CvMat *R1 = cvCreateMat(3, 3, CV_64F);
        CvMat *R2 = cvCreateMat(3, 3, CV_64F);
        CvMat *P1 = cvCreateMat(3, 4, CV_64F);
        CvMat *P2 = cvCreateMat(3, 4, CV_64F);
        CvMat *Q = cvCreateMat(4, 4, CV_64F);
        cvSetZero(M1);
        cvSetZero(M2);
        cvSetZero(D1);
        cvSetZero(D2);
        cvSetZero(R1);
        cvSetZero(R2);
        cvSetZero(P1);
        cvSetZero(P2);
        cvSetZero(R);
        cvSetZero(T);
        cvSetZero(Q);
        
        M1->data.db[0] = FX0;
        M1->data.db[2] = CX0;
        M1->data.db[4] = FY0;
        M1->data.db[5] = CY0;
        M2->data.db[0] = FX1;
        M2->data.db[2] = CX1;
        M2->data.db[4] = FY1;
        M2->data.db[5] = CY1;
        R->data.db[0] = R11;
        R->data.db[1] = R12;
        R->data.db[2] = R13;
        R->data.db[3] = R21;
        R->data.db[4] = R22;
        R->data.db[5] = R23;
        R->data.db[6] = R31;
        R->data.db[7] = R32;
        R->data.db[8] = R33;
        T->data.db[0] = -T1;
        T->data.db[1] = -T2;
        T->data.db[2] = -T3;
        D1->data.db[0] = D01;
        D1->data.db[1] = D02;
        D1->data.db[2] = D03;
        D1->data.db[3] = D04;
        D1->data.db[4] = D05;
        D2->data.db[0] = D11;
        D2->data.db[1] = D12;
        D2->data.db[2] = D13;
        D2->data.db[3] = D14;
        D2->data.db[4] = D15;

        cvStereoRectify( M1, M2, D1, D2, imageSize,
            R, T,
            R1, R2, P1, P2, Q,
            CV_CALIB_ZERO_DISPARITY,
            1, imageSize, &(roi[0]), &(roi[1]));
        //Precompute maps for cvRemap() - part of stereo rectification
        cvInitUndistortRectifyMap(M1,D1,R1,P1,mx[0],my[0]);
        cvInitUndistortRectifyMap(M2,D2,R2,P2,mx[1],my[1]);

        //Initialise the capture for both cameras (videos)
	for (int i = 0; i < NUM_CAMERAS; ++i)
	{
		camera[i].init_capture(cam_index[i]);	//+1 is so we dont use the laptop webcam
	}        
        
        //Initialise binomial lookuptable
        for (int n = 0; n < 50; n++)
        {
            for (int k = 0; k < 50; k++)
            {
                if (k == 0)
                    binom[n][k] = 1;
                else if (n == 0)
                    binom[n][k] = 0;
                else
                    binom[n][k] = binom[n-1][k]+binom[n-1][k-1];
            }
        }


        //Initialise image data structs
        CvSize frame_size = cvSize(PIC_WIDTH,PIC_HEIGHT);
        last_image[0] = cvCreateImage( frame_size, IPL_DEPTH_8U, 1 );
        last_image[1] = cvCreateImage( frame_size, IPL_DEPTH_8U, 1 );
        last_image_raw[0] = cvCreateImage( frame_size, IPL_DEPTH_8U, 3 );
        last_image_raw[1] = cvCreateImage( frame_size, IPL_DEPTH_8U, 3 );
        last_image_track[0] = cvCreateImage( frame_size, IPL_DEPTH_8U, 1 );
        last_image_track[1] = cvCreateImage( frame_size, IPL_DEPTH_8U, 1 );
        image_color[0] = cvCreateImage( frame_size, IPL_DEPTH_8U, 3 );
        image_color[1] = cvCreateImage( frame_size, IPL_DEPTH_8U, 3 );
        image[0] = cvCreateImage( frame_size, IPL_DEPTH_8U, 1 );
        image[1] = cvCreateImage( frame_size, IPL_DEPTH_8U, 1 );
        last_image_color[0] = cvCreateImage( frame_size, IPL_DEPTH_8U, 3 );
        last_image_color[1] = cvCreateImage( frame_size, IPL_DEPTH_8U, 3 );
        
        //Initialise windows
        cvNamedWindow("Left");
        cvNamedWindow("Right");
        cvMoveWindow("Left",0,0);
        cvMoveWindow("Right",320,0);

        //Get frames from camera
        for (int n = 0; n < FRAMES_TO_SKIP; n++)
        {
            for (int i = 0; i < NUM_CAMERAS; ++i)
            {
                    image_raw[i] = camera[i].get_frame();
            }
        }
        
        //Get extra from either left or right camera based on what we think the lag is
        //(Need frames to be synchronised)
        for (int i = 0; i < LEFT_IMAGE_LAG; ++i)
        {
                image_raw[0] = camera[0].get_frame();
        }
        if (LEFT_IMAGE_LAG < 0)
        {
            for (int i = 0; i < -LEFT_IMAGE_LAG; ++i)
            {
                    image_raw[1] = camera[1].get_frame();
            }
        }
        //Stereo rectify initial image
        cvRemap(image_raw[0], image_color[0], mx[0], my[0]);
        cvRemap(image_raw[1], image_color[1], mx[1], my[1]);
        cvCopy(image_raw[0],image_color[0],NULL);
        cvCopy(image_raw[1],image_color[1],NULL);

        //Init RNG
        CvRandState randstate;
        cvRandInit(&randstate,0.1,0.0,0xffffffff,CV_RAND_NORMAL);
        
        //Mats to store measurement vectors of different sizes
        CvMat *z8 = cvCreateMat(8,1,CV_64FC1);
        CvMat *z16 = cvCreateMat(19,1,CV_64FC1);
        CvMat *z = cvCreateMat(35,1,CV_64FC1);
        CvMat *z1 = cvCreateMat(4,1,CV_64FC1);
        CvMat *z2 = cvCreateMat(4,1,CV_64FC1);
        CvMat *z3 = cvCreateMat(4,1,CV_64FC1);
        CvMat *z4 = cvCreateMat(4,1,CV_64FC1);
        CvMat *z_left = cvCreateMat(16,1,CV_64FC1);
        CvMat *z_right = cvCreateMat(16,1,CV_64FC1);
        
        std::vector<double> state_limits;       //Stores the 'limits' of each curves (ie. what we've observed of it; full observation is 0 to 1)

        //Curve control point transformation matrices
        CvMat * A1 = cvCreateMat(4,4,CV_64FC1);
        CvMat * A2 = cvCreateMat(4,4,CV_64FC1);
	CvMat * A1l = cvCreateMat(4,4,CV_64FC1);
	CvMat * A2l = cvCreateMat(4,4,CV_64FC1);
	CvMat * B1l = cvCreateMat(4,4,CV_64FC1);
	CvMat * B2l = cvCreateMat(4,4,CV_64FC1);
	CvMat * A1r = cvCreateMat(4,4,CV_64FC1);
	CvMat * A2r = cvCreateMat(4,4,CV_64FC1);
	CvMat * B1r = cvCreateMat(4,4,CV_64FC1);
	CvMat * B2r = cvCreateMat(4,4,CV_64FC1);
        CvMat * I4 = cvCreateMat(4,4,CV_64FC1);
        cvSetZero(I4);
        I4->data.db[0] = 1.0;
        I4->data.db[5] = 1.0;
        I4->data.db[10] = 1.0;
        I4->data.db[15] = 1.0;

        int counter = 0;
        
       
        //Initialise guess params (just in case the first curve fit fails!)
        double params[19];
        for (int i = 0; i < 16; i++)
            params[i] = 0.0;
        params[16] = 0.0;
        params[17] = -0.2;
        params[18] = -2.0;
        double last_params[19];
        for (int i = 0; i < 16; i++)
            params[i] = 0.0;
        params[16] = 0.0;
        params[17] = -0.2;
        params[18] = -2.0;

        //Flags to determine what to do with the left and right curves (state machine)
        char left_assoc_flag = ADD_FIRST_STATES;
        char right_assoc_flag = ADD_FIRST_STATES;
        bool partial_curve_observed[2] = {false, false};
        bool first_time = true;

        std::vector<int> left_curve_nums;
        std::vector<int> right_curve_nums;
        std::vector<int> curves_updated_last;
        curves_updated_last.push_back(0);
        curves_updated_last.push_back(1);
        std::vector<int> curves_to_update;
        int num_map_curves = 0;
        
        double t_split[6];
        double last_t_split[6];

        bool edges_detected = true;
        bool reset_data_assoc = true;
        bool valid_measurement = true;
        bool valid_last_measurement = true;
        int frames_since_good_measurement = 0;
        bool use_points = false;        //Flag we set on the fly to decide if we need points or not
        
        
        double t_split_default[] = {0.0,0.0,0.0};
        double * t_splitL = &t_split_default[0];
        double * t_splitR = &t_split_default[0];
        
        
//MAIN LOOP OVER VIDEO FRAMES
        
       
        int count = 0;
        while(1)
	{
            count++;
            //cout << valid_measurement << " " << frames_since_good_measurement << endl;
            if(valid_measurement)
            {
                frames_since_good_measurement = 1;
                use_points = false;
            }
            else
                frames_since_good_measurement++;
            if (edges_detected = false || first_time)
            {
                reset_data_assoc = true;
                valid_measurement = true;
            }
            else if (frames_since_good_measurement >= NUM_FRAMES_DA_RESET)
            {
                reset_data_assoc = true;
                //use_points = true;
                use_points = false;
            }
            else
                reset_data_assoc = false;
            
            valid_last_measurement = valid_measurement;

            cvCopy(image_raw[0], last_image_raw[0], NULL);
            cvCopy(image_raw[1], last_image_raw[1], NULL);
            cvCvtColor(last_image_raw[0],last_image[0],CV_RGB2GRAY);
            cvCvtColor(last_image_raw[1],last_image[1],CV_RGB2GRAY);
            cvCopy(image_color[0], last_image_color[0], NULL);
            cvCopy(image_color[1], last_image_color[1], NULL);

            //Do the REAL VISION stuff first
            for (int n = 0; n < FRAME_INTERVAL; n++)
            {
		for (int i = 0; i < NUM_CAMERAS; ++i)
		{
                        cvCopy(image[i], last_image_track[i],NULL);
			image_raw[i] = camera[i].get_frame();
                        cvRemap(image_raw[i], image_color[i], mx[i], my[i]);
                        cvCopy(image_color[i],image_raw[i],NULL);
                        //cvCopy(image_raw[i],image_color[i],NULL);
                        cvCvtColor(image_raw[i],image[i],CV_RGB2GRAY);
                }
                
                //Track each curve too (if last measurement was valid)
                if (!reset_data_assoc)
                {
                        curveMatcher[0].singleFrameTrack(last_image_track,image, roi[0]);
                        curveMatcher[1].singleFrameTrack(last_image_track,image, roi[0]);
                }
                counter++;
            }
            int n_pts_existing = 0;
            int n_pts_new = 0;
            double point_meas_existing[150];
            double point_meas_new[150];
            double planar_pose_meas[3];
            int correspondences[50];
            
            cvCopy(image_raw[0], image_color[0], NULL);
            cvCopy(image_raw[1], image_color[1], NULL);
            //use_points = true;
            
            if(!first_time)
            {
                //Get point measurements (only if not first frame and previous curve measurement was invalid)
                if (use_points)
                        pointFeatures->getPointMeasurements(&last_image[0],&image[0], &image_color[0], &point_meas_new[0],&n_pts_new,&point_meas_existing[0],&n_pts_existing, &correspondences[0], &planar_pose_meas[0]);

                EKF->PredictKF();       //Also perform EKF predict step if not first time
            }



            //Find the edge points corresponding to curves in the images
            CvPoint2D32f map_endpts[] = {curveMatcher[0].map_endpt_tracked,curveMatcher[1].map_endpt_tracked};
            for (int i = 0; i < NUM_CAMERAS; ++i)
            {
                    features[i].find_features(image_raw[i],seg[i],i,&(roi[i]),&map_endpts[0]);
                    curveMatcher[i].map_endpt_tracked = map_endpts[i];
            }
#ifndef NO_SLAM
            
            display_GUI->copy_images(&(image[0]));

            for (int i = 0; i < 19; i++)
                last_params[i] = params[i];



//GET EDGE POINTS AND GROUP THEM
            //Extract edge points from both images
            std::vector<CvPoint> ** featuresL =features[LEFT].return_features();
            std::vector<CvPoint> ** featuresR =features[RIGHT].return_features();

            //Below: hacky stuff to make the lengths the same, etc! (same amount of each curve observed in both images, etc)
            int left_y_cutoff[] = {MIN(TOP_Y_CUTOFF,curveMatcher[0].map_endpt_tracked.y),MIN(TOP_Y_CUTOFF,curveMatcher[1].map_endpt_tracked.y)};
            if(first_time)
            {
                left_y_cutoff[0] = TOP_Y_CUTOFF;
                left_y_cutoff[1] = TOP_Y_CUTOFF;                    
            }
            CvMat * featuresLeftImage[2];
            CvMat * featuresRightImage[2];
            double tracked_endpt_y[] = {curveMatcher[0].map_endpt_tracked.y,curveMatcher[1].map_endpt_tracked.y};
            //cout << "1\n";
            edges_detected = curveFitter->cleanup_and_group_edges(featuresL,featuresR,&featuresLeftImage[0],&featuresRightImage[0], reset_data_assoc, &left_y_cutoff[0], &tracked_endpt_y[0]);
            //cout << "2\n";
            
            CvMat * state_current = EKF->getState();
            
            //Only proceed if the above was successful and we have edges to work with
            if (!edges_detected)
            {
                cout << "Edges not detected!\n";
                valid_measurement = false;
            }
            else
            {

                double euler[3], euler_last[3];
                double translation[3], translation_last[3];
                double p[16], p_left[16], p_right[16];
                double p_last[16], p_left_last[16], p_right_last[16];

//PERFORM CURVE FITTING AND STORE THE PARAMS IN 'params'
                curveFitter->fit_curve(&(params[0]), featuresLeftImage, featuresRightImage, display_GUI);
                //valid_measurement = EKF->CheckValidMeasurement(params[16],params[17],params[18],frames_since_good_measurement);
                valid_measurement = true;
                if (curveFitter->fitting_error > ERROR_THRESHOLD)
                {
                    valid_measurement = false;
                    cout << "Curve fitting threshold exceeded!\n";
                }       
                else
                {
                    for (int i = 0; i < 19; i++)
                    {
                        if (isnan(params[i]))
                        {
                            valid_measurement = false;
                            cout << "Curve fitting produced NaN params!\n";
                            break;
                        }
                    }
                }
                
                for (int i = 0; i < 16; i++)
                {
                    if (fabs(params[i]) > 50.0)
                    {
                        valid_measurement = false;
                        cout << "Curve fitting params out of range!\n";
                        break;
                    }
                }
                if (params[18] > 0.0)
                {
                    valid_measurement = false;
                    cout << "Negative height from curve fitting!\n";
                }

                if(first_time)
                    valid_measurement = true;

                double theta = params[16];
                double phi = params[17];
                double height = params[18];

                if(valid_measurement)
                {
                    euler[0] = 0.0;
                    euler[1] = params[16];
                    euler[2] = params[17];
                    translation[0] = 0.0;
                    translation[1] = 0.0;
                    translation[2] = params[18];

                    //Convert measured curve to image space, then display them on the images
                    control2coeffs(params,p);
                    control2coeffs(&(params[8]),&(p[8]));
                    poly_earth2image(&(p[0]), p_left, p_right, euler, translation);
                    poly_earth2image(&(p[8]), &(p_left[8]), &(p_right[8]), euler, translation);

                    euler_last[0] = 0.0;
                    euler_last[1] = last_params[16];
                    euler_last[2] = last_params[17];
                    translation_last[0] = 0.0;
                    translation_last[1] = 0.0;
                    translation_last[2] = last_params[18];

                    control2coeffs(last_params,p_last);
                    control2coeffs(&(last_params[8]),&(p_last[8]));
                    poly_earth2image(&(p_last[0]), p_left_last, p_right_last, euler_last, translation_last);
                    poly_earth2image(&(p_last[8]), &(p_left_last[8]), &(p_right_last[8]), euler_last, translation_last);


                    display_GUI->display_images(image[0], image[1], featuresLeftImage, featuresRightImage, p_left, p_right,image_color[0], image_color[1]);


// DETERMINE DATA ASSOCIATION (getMatchT) TO FIND THE T-VALUES TO SPLIT AT
                    for (int i = 0; i < 6; i++)
                    {
                        last_t_split[i] = t_split[i];
                    }

                    t_splitL = curveMatcher[0].getMatchT( featuresLeftImage[0], &(last_image[0]), &(last_image_track[0]),&(image[0]),&(last_image_color[0]),&(image_color[0]),&(last_params[0]), &(params[0]), 0, &left_assoc_flag, reset_data_assoc);
                    t_splitR = curveMatcher[1].getMatchT( featuresLeftImage[1], &(last_image[0]), &(last_image_track[0]),&(image[0]),&(last_image_color[0]),&(image_color[0]),&(last_params[0]), &(params[0]), 1, &right_assoc_flag, reset_data_assoc);

                    for (int i = 0; i < 3; i++)
                    {
                        t_split[2*i] = t_splitL[i];
                        t_split[2*i+1] = t_splitR[i];
                    }

                    //cout << "T SPLIT PTS\n";
                    //cout << t_split[0] << " " << t_split[2] << " "<< t_split[4] << " " << t_split[1] << " "<< t_split[3] << " " << t_split[5] << endl;
                }
                
//Determine state based on data assoc parameter (if valid measurement)
                if(valid_measurement && !first_time)
                {
                    if (t_splitL[0] < -0.5)
                    {
                        left_assoc_flag = ONLY_ADD_STATE;
                        curveMatcher[0].updateMapCurves();
                        partial_curve_observed[0] = false;
                    }
                    else if (t_splitL[0] < 0.05 && t_splitL[1] > 0.95 && t_splitL[2] < 0.05)
                    {
                        if (partial_curve_observed[0])
                        {
                            left_assoc_flag = ONLY_UPDATE_ONE_STATE_PREV;
                        }
                        else
                        {
                            left_assoc_flag = ONLY_UPDATE_ONE_STATE;
                        }

                    }
                    else if (t_splitL[0] > 0.95 && t_splitL[1] < 0.05 && t_splitL[2] > 0.95)
                    {
                        if (partial_curve_observed[0])
                        {
                            left_assoc_flag = ONLY_UPDATE_ONE_STATE;
                        }
                        else
                        {
                            left_assoc_flag = ONLY_ADD_STATE;
                        }

                        partial_curve_observed[0] = false;
                        curveMatcher[0].updateMapCurves();

                    }
                    else
                    {
                        if(partial_curve_observed[0])
                        {
                            left_assoc_flag = UPDATE_TWO_STATES;
                        }
                        else
                        {
                            left_assoc_flag = ADD_AND_UPDATE_STATES;
                        }
                        partial_curve_observed[0] = true;

                    }


                    if (t_splitR[0] < -0.5)
                    {
                        right_assoc_flag = ONLY_ADD_STATE;
                        curveMatcher[1].updateMapCurves();
                        partial_curve_observed[1] = false;
                    }
                    else if (t_splitR[0] < 0.05 && t_splitR[1] > 0.95 && t_splitR[2] < 0.05)
                    {
                        if (partial_curve_observed[1])
                        {
                            right_assoc_flag = ONLY_UPDATE_ONE_STATE_PREV;
                        }
                        else
                        {
                            right_assoc_flag = ONLY_UPDATE_ONE_STATE;
                        }

                    }
                    else if (t_splitR[0] > 0.95 && t_splitR[1] < 0.05 && t_splitR[2] > 0.95)
                    {
                        if (partial_curve_observed[1])
                        {
                            right_assoc_flag = ONLY_UPDATE_ONE_STATE;
                        }
                        else
                        {
                            right_assoc_flag = ONLY_ADD_STATE;
                        }

                        partial_curve_observed[1] = false;
                        curveMatcher[1].updateMapCurves();

                    }
                    else
                    {
                        if(partial_curve_observed[1])
                        {
                            right_assoc_flag = UPDATE_TWO_STATES;
                        }
                        else
                        {
                            right_assoc_flag = ADD_AND_UPDATE_STATES;
                        }
                        partial_curve_observed[1] = true;

                    }
                }
                else if (first_time)
                {
                    left_assoc_flag = ADD_FIRST_STATES;
                    right_assoc_flag = ADD_FIRST_STATES;
                    first_time = false;
                }

                //Plot features on images
                for( int i = 0; i < featuresLeftImage[LEFT]->rows/2; i++)
                {
                        cvCircle(image_color[LEFT],cvPoint(featuresLeftImage[LEFT]->data.db[2*i],featuresLeftImage[LEFT]->data.db[2*i+1]),1,CV_RGB(0,0,0));
                }
                for( int i = 0; i < featuresLeftImage[RIGHT]->rows/2; i++)
                {
                        cvCircle(image_color[LEFT],cvPoint(featuresLeftImage[RIGHT]->data.db[2*i],featuresLeftImage[RIGHT]->data.db[2*i+1]),1,CV_RGB(0,0,0));
                }
                for( int i = 0; i < featuresRightImage[LEFT]->rows/2; i++)
                {
                        cvCircle(image_color[RIGHT],cvPoint(featuresRightImage[LEFT]->data.db[2*i],featuresRightImage[LEFT]->data.db[2*i+1]),1,CV_RGB(0,0,0));
                }
                for( int i = 0; i < featuresRightImage[RIGHT]->rows/2; i++)
                {
                        cvCircle(image_color[RIGHT],cvPoint(featuresRightImage[RIGHT]->data.db[2*i],featuresRightImage[RIGHT]->data.db[2*i+1]),1,CV_RGB(0,0,0));
                }
                
                cvShowImage("Left",image_color[LEFT]);
                cvShowImage("Right",image_color[RIGHT]);
                cvShowImage("Last Left",last_image_color[LEFT]);
                cvShowImage("Last Right",last_image_color[RIGHT]);


//Figure out from data assoc what to do (state machine)
// Determine whether to just add states, or update, or both
                if (left_assoc_flag == ADD_FIRST_STATES && right_assoc_flag == ADD_FIRST_STATES && valid_measurement)
                {
                    for (int i = 0; i < 4; i++)
                    {
                        z16->data.db[i] = params[2*i];
                        z16->data.db[i+4] = params[2*i+1];
                        z16->data.db[i+8] = params[2*(i+4)];
                        z16->data.db[i+12] = params[2*(i+4)+1];
                        z->data.db[i] = params[2*i];
                        z->data.db[i+4] = params[2*i+1];
                        z->data.db[i+8] = params[2*(i+4)];
                        z->data.db[i+12] = params[2*(i+4)+1];
                    }
                            z16->data.db[16] = height;
                            z16->data.db[17] = phi;
                            z16->data.db[18] = theta;
                    EKF->AddFirstStates(z16);
                    state_limits.push_back(1.0);
                    state_limits.push_back(1.0);
                    left_curve_nums.push_back(0);
                    right_curve_nums.push_back(1);
                    num_map_curves+=2;
                }

                //Check if the measurement is valid, otherwise, ignore
                else if (valid_measurement)
                {
                    //Initialise update params (will be set as necessary))
                    int num_curves_to_update = 0;
                    curves_to_update.clear();
                    std::vector<CvMat *> correspondence_matrices;

                    CvMat * tempx = cvCreateMat(4,1,CV_64FC1);
                    CvMat * tempy = cvCreateMat(4,1,CV_64FC1);


                    //Depending on left and right assoc_flags, decide whether or not to split the measurement into two!
                    if (left_assoc_flag & (UPDATE_TWO_STATES|ADD_AND_UPDATE_STATES))
                    {
                        //Split left into two curves
                        for (int i = 0; i < 4; i++)
                        {
                            tempx->data.db[i] = params[2*i];
                            tempy->data.db[i] = params[2*i+1];
                        }

                        EKF->GetSplitMatrices(t_splitL[1],A1,A2);
                        cvMatMul(A1,tempx,z1);                    //Curve 1 x and y
                        cvMatMul(A1,tempy,z2);
                        cvMatMul(A2,tempx,z3);                    //Curve 2 x and y
                        cvMatMul(A2,tempy,z4);

                        for (int i = 0; i < 4; i++)
                        {
                            z_left->data.db[i] = z1->data.db[i];
                            z_left->data.db[i+4] = z2->data.db[i];
                            z_left->data.db[i+8] = z3->data.db[i];
                            z_left->data.db[i+12] = z4->data.db[i];
                        }
                        EKF->GetSplitMatrices(t_splitL[0],A1l,A2l);
                        EKF->GetSplitMatrices(t_splitL[2],B1l,B2l);
                    }
                    if (right_assoc_flag & (UPDATE_TWO_STATES|ADD_AND_UPDATE_STATES))
                    {
                        //Split RIGHT into two curves
                        for (int i = 0; i < 4; i++)
                        {
                            tempx->data.db[i] = params[2*i+8];
                            tempy->data.db[i] = params[2*i+9];
                        }

                        EKF->GetSplitMatrices(t_splitR[1],A1,A2);
                        cvMatMul(A1,tempx,z1);                    //Curve 1 x and y
                        cvMatMul(A1,tempy,z2);
                        cvMatMul(A2,tempx,z3);                    //Curve 2 x and y
                        cvMatMul(A2,tempy,z4);

                        for (int i = 0; i < 4; i++)
                        {
                            z_right->data.db[i] = z1->data.db[i];
                            z_right->data.db[i+4] = z2->data.db[i];
                            z_right->data.db[i+8] = z3->data.db[i];
                            z_right->data.db[i+12] = z4->data.db[i];
                        }

                        EKF->GetSplitMatrices(t_splitR[0],A1r,A2r);
                        EKF->GetSplitMatrices(t_splitR[2],B1r,B2r);
                    }


                    //UPDATE AND ADD LEFT CURVES
                    switch (left_assoc_flag)
                    {
                        case ONLY_ADD_STATE:
                        {
                            for (int i = 0; i < 4; i++)
                            {
                                z8->data.db[i] = params[2*i];
                                z8->data.db[i+4] = params[2*i+1];
                                z->data.db[i] = params[2*i];
                                z->data.db[i+4] = params[2*i+1];
                            }
                            
                            EKF->AddNewCurve(z8,I4);
                            state_limits.push_back(1.0);
                            left_curve_nums.push_back(num_map_curves);
                            num_map_curves++;
                            break;
                        }
                        case ONLY_UPDATE_ONE_STATE:
                        {
                            for (int i = 0; i < 4; i++)
                            {
                                z->data.db[i+8*num_curves_to_update] = params[2*i];
                                z->data.db[i+4+8*num_curves_to_update] = params[2*i+1];
                            }

                            correspondence_matrices.push_back(I4);
                            state_limits.at(*(left_curve_nums.end()-1)) = 1.0;
                            curves_to_update.push_back(*(left_curve_nums.end()-1));
                            num_curves_to_update++;
                            break;
                        }
                        case ONLY_UPDATE_ONE_STATE_PREV:
                        {
                            for (int i = 0; i < 4; i++)
                            {
                                z->data.db[i+8*num_curves_to_update] = params[2*i];
                                z->data.db[i+4+8*num_curves_to_update] = params[2*i+1];
                            }

                            correspondence_matrices.push_back(I4);
                            state_limits.at(*(left_curve_nums.end()-2)) = 1.0;
                            curves_to_update.push_back(*(left_curve_nums.end()-2));
                            num_curves_to_update++;
                            break;
                        }
                        case ADD_AND_UPDATE_STATES:
                        {
                            //Add the first curve to 'update list'
                            for (int i = 0; i < 8; i++)
                            {
                                z->data.db[i+8*num_curves_to_update] = z_left->data.db[i];
                            }

                            correspondence_matrices.push_back(A2l);
                            state_limits.at(*(left_curve_nums.end()-1)) = 1.0;
                            curves_to_update.push_back(*(left_curve_nums.end()-1));
                            num_curves_to_update++;


                            //Add second curve as new state (but only if enough of it has been observed)
                            if (t_splitL[2] > CURVE_MIN_OBS)
                            {
                                for (int i = 0; i < 8; i++)
                                    z8->data.db[i] = z_left->data.db[i+8];

                                EKF->AddNewCurve(z8,B1l);
                                state_limits.push_back(t_splitL[2]);
                                left_curve_nums.push_back(num_map_curves);
                                num_map_curves++;
                            }
                            else
                                partial_curve_observed[0] = false;
                            break;
                        }
                        case UPDATE_TWO_STATES:
                        {
                            for (int i = 0; i < 16; i++)
                            {
                                z->data.db[i+8*num_curves_to_update] = z_left->data.db[i];
                            }

                            correspondence_matrices.push_back(A2l);
                            correspondence_matrices.push_back(B1l);
                            curves_to_update.push_back(*(left_curve_nums.end()-2));
                            curves_to_update.push_back(*(left_curve_nums.end()-1));
                            state_limits.at(*(left_curve_nums.end()-2)) = 1.0;
                            state_limits.at(*(left_curve_nums.end()-1)) = t_splitL[2];
                            num_curves_to_update+=2;
                            break;
                        }
                        case DISCARD_MEASUREMENT:
                        {
                            break;
                        }
                    }

                    //UPDATE AND ADD RIGHT CURVES
                    switch (right_assoc_flag)
                    {
                        case ONLY_ADD_STATE:
                        {
                            for (int i = 0; i < 4; i++)
                            {
                                z8->data.db[i] = params[2*(i+4)];
                                z8->data.db[i+4] = params[2*(i+4)+1];
                                z->data.db[i+8] = params[2*(i+4)];
                                z->data.db[i+12] = params[2*(i+4)+1];
                            }
                            
                            EKF->AddNewCurve(z8,I4);
                            state_limits.push_back(1.0);
                            right_curve_nums.push_back(num_map_curves);
                            num_map_curves++;
                            break;
                        }
                        case ONLY_UPDATE_ONE_STATE:
                        {
                            for (int i = 0; i < 4; i++)
                            {
                                z->data.db[i+8*num_curves_to_update] = params[2*(i+4)];
                                z->data.db[i+4+8*num_curves_to_update] = params[2*(i+4)+1];
                            }

                            correspondence_matrices.push_back(I4);
                            state_limits.at(*(right_curve_nums.end()-1)) = 1.0;
                            curves_to_update.push_back(*(right_curve_nums.end()-1));
                            num_curves_to_update++;
                            break;
                        }
                        case ONLY_UPDATE_ONE_STATE_PREV:
                        {
                            for (int i = 0; i < 4; i++)
                            {
                                z->data.db[i+8*num_curves_to_update] = params[2*(i+4)];
                                z->data.db[i+4+8*num_curves_to_update] = params[2*(i+4)+1];
                            }

                            correspondence_matrices.push_back(I4);
                            state_limits.at(*(right_curve_nums.end()-2)) = 1.0;
                            curves_to_update.push_back(*(right_curve_nums.end()-2));
                            num_curves_to_update++;
                            break;
                        }
                        case ADD_AND_UPDATE_STATES:
                        {                    
                            //Add the first curve to 'update list'
                            for (int i = 0; i < 8; i++)
                            {
                                z->data.db[i+8*num_curves_to_update] = z_right->data.db[i];
                            }

                            correspondence_matrices.push_back(A2r);
                            state_limits.at(*(right_curve_nums.end()-1)) = 1.0;
                            curves_to_update.push_back(*(right_curve_nums.end()-1));
                            num_curves_to_update++;


                            //Add second curve as new state (but only if enough of it has been observed)
                            if (t_splitR[2] > CURVE_MIN_OBS)
                            {
                                for (int i = 0; i < 8; i++)
                                        z8->data.db[i] = z_right->data.db[i+8];

                                EKF->AddNewCurve(z8,B1r);
                                state_limits.push_back(t_splitR[2]);
                                right_curve_nums.push_back(num_map_curves);
                                num_map_curves++;
                            }
                            else
                                partial_curve_observed[1] = false;
                            break;
                        }
                        case UPDATE_TWO_STATES:
                        {
                            for (int i = 0; i < 16; i++)
                            {
                                z->data.db[i+8*num_curves_to_update] = z_right->data.db[i];
                            }

                            correspondence_matrices.push_back(A2r);
                            correspondence_matrices.push_back(B1r);
                            curves_to_update.push_back(*(right_curve_nums.end()-2));
                            curves_to_update.push_back(*(right_curve_nums.end()-1));
                            state_limits.at(*(right_curve_nums.end()-2)) = 1.0;
                            state_limits.at(*(right_curve_nums.end()-1)) = t_splitR[2];
                            num_curves_to_update+=2;
                            break;
                        }
                        case DISCARD_MEASUREMENT:
                        {
                            break;
                        }
                    }

//UPDATE EXISTING CURVES/POINTS IF NEEDED
                    if (num_curves_to_update > 0 || n_pts_existing > 0)
                    {
                        //num_curves_to_update = 0;
                        z->data.db[num_curves_to_update*8] = height;
                        z->data.db[num_curves_to_update*8+1] = phi;
                        z->data.db[num_curves_to_update*8+2] = theta;
                        //cout << "n pts existing: " << n_pts_existing << endl;
                        //n_pts_existing = MIN(10,n_pts_existing);
                        //EKF->UpdateNCurvesAndPoints(z, num_curves_to_update, &(correspondence_matrices),&(curves_to_update),&point_meas_existing[0],&correspondences[0],n_pts_existing);
                        EKF->UpdateNCurvesAndPoints(z, num_curves_to_update, &(correspondence_matrices),&(curves_to_update),&point_meas_existing[0],&correspondences[0],0);
                        //EKF->UpdatePoints(&point_meas_existing[0],&correspondences[0],n_pts_existing);
                    }

                }
//UPDATE ONLY POINTS IF CURVES ARE A FAIL
                if (n_pts_existing > 0)
                    EKF->UpdatePoints(&point_meas_existing[0],&correspondences[0],n_pts_existing);
                    
                curves_updated_last.clear();
                for (int i = 0; i < curves_to_update.size(); i++)
                    curves_updated_last.push_back(curves_to_update.at(i));
                /*cout << "Left curve nums: ";
                for (int i = 0; i < left_curve_nums.size(); i++)
                    cout << left_curve_nums.at(i) << " ";
                cout << endl;
                cout << "Right curve nums: ";
                for (int i = 0; i < right_curve_nums.size(); i++)
                    cout << right_curve_nums.at(i) << " ";
                cout << endl;
                cout << "Curves to update: ";
                for (int i = 0; i < curves_to_update.size(); i++)
                    cout << curves_to_update[i] << " ";
                cout << endl;*/


            }
            
//Add any new point measurements we've made
            EKF->AddNewPoints(&point_meas_new[0], n_pts_new);
            

            //Display curves
            state_current = EKF->getState();
            display_GUI->generate_map(state_current,&state_limits,z, &(EKF->curve_inds), &(EKF->point_inds), EKF->num_curves, EKF->num_points);
            //Print robot pose
            //cout << "X: " << state_current->data.db[0] << "\n" << "Y: " << state_current->data.db[1] << "\n" << "Z: " << state_current->data.db[2] << "\n"  << "Pitch: " << state_current->data.db[4]*180.0/PI << "\n" << "Roll: " << state_current->data.db[3]*180.0/PI << "\n" << "Yaw: " << state_current->data.db[5]*180.0/PI << "\n";
#endif
            /*cout << "Curve inds:\n";
            for (int i = 0; i < EKF->curve_inds.size(); i++)
                cout << EKF->curve_inds.at(i) << " ";
            cout << endl;
            cout << "Point inds:\n";
            for (int i = 0; i < EKF->point_inds.size(); i++)
                cout << EKF->point_inds.at(i) << " ";
            cout << endl;*/

            
            //Timing stuff, can use this when worrying about speed
            int frameTime = camera[0].getTime()+camera[1].getTime();
            int featureTime = features[0].getTime()+features[1].getTime();
            int dataAssocTime = curveMatcher[0].getTime()+curveMatcher[1].getTime();
            int curveFitTime = curveFitter->getTime();
            int EKFTime = EKF->getTime();
            int displayTime = display_GUI->getTime();

            /*cout << "Frame grab: " << frameTime << " msec\t";
            cout << "Feature detection: " << featureTime << " msec\t";
            cout << "Data Assoc: " << dataAssocTime << " msec\t";
            cout << "Curve Fit: " << curveFitTime << " msec\t";
            cout << "EKF: " << EKFTime << " msec\t";
            cout << "Display: " << displayTime << " msec\t";*/

            EKF->resetTime();
            curveFitter->resetTime();
            display_GUI->resetTime();
            for (int i = 0; i < NUM_CAMERAS; ++i)
            {
                camera[i].resetTime();
                features[i].resetTime();
                curveMatcher[i].resetTime();
            }

            gettimeofday(&stop, NULL);
                elapsedTime = (stop.tv_sec*1000.0 + stop.tv_usec/1000.0) -
                        (start.tv_sec*1000.0 + start.tv_usec/1000.0);
                        //cout << "TOTAL: " << elapsedTime << endl;
            gettimeofday(&start, NULL);
            
            cvWaitKey(0);
        }

	return 0;
}

