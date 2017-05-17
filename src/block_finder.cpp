#include <ros/ros.h>//ROSを使用するときは必ずincludeする。
#include <geometry_msgs/Pose2D.h>//PoseやTwistを使用する場合は適宜includeする。
#include <std_msgs/String.h>
#include <image_transport/image_transport.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>
#include <tf2/LinearMath/Quaternion.h>
#include <geometry_msgs/PoseStamped.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/calib3d/calib3d.hpp>
#include <vector>
#include <pcl_ros/transforms.h>//#include <Eigen/Geometry>
#include <image_geometry/pinhole_camera_model.h>//カメラモデルを利用するため

using namespace std;
using namespace cv;

static const string WINDOW_O = "Original";
static const string WINDOW_T = "Temp";
static const string WINDOW_R = "Result";
static int threshold_bin = 160;

class BlockFinder
{
	ros::NodeHandle nh_;
	image_transport::ImageTransport it;
	image_transport::Subscriber image_sub_;
	image_transport::Publisher image_pub_;
	ros::Subscriber info_sub_;
	ros::Publisher pose_pub_;
	tf::TransformListener tf_listener_;
	tf::TransformBroadcaster tf_broadcaster_;

	image_geometry::PinholeCameraModel cam_model_;
	
	std::string fixed_frame;
	std::string camera_frame;
	std::string target_frame;

	cv::Mat ideal_points_;

public:


	BlockFinder(): it(nh_)
{
		image_sub_ = it.subscribe("/usb_cam_node/image_raw", 1, &BlockFinder::imageCb, this);
		info_sub_ = nh_.subscribe("/usb_cam/camera_info", 1, &CalibrateKinectCheckerboard::infoCallback, this);
		image_pub_ = it.advertise("/block_finder/image_block", 1);
		pose_pub_ = nh_.advertise<geometry_msgs::Pose2D>("/block_finder/pose", 1);		

		nh_.param<std::string>("fixed_frame", fixed_frame, "/base_link");
		nh_.param<std::string>("camera_frame", camera_frame, "/camera_link");
		nh_.param<std::string>("target_frame", target_frame, "/calibration_pattern");

		namedWindow(WINDOW_O);
		namedWindow(WINDOW_T);
		namedWindow(WINDOW_R);
		moveWindow(WINDOW_O, 0, 0);
		moveWindow(WINDOW_T, 640, 0);
		moveWindow(WINDOW_R, 1280, 0);
		createTrackbar("Subtracter", WINDOW_T, &threshold_bin, 255);
}

	~BlockFinder()
	{
		destroyWindow(WINDOW_O);
		destroyWindow(WINDOW_T);
		destroyWindow(WINDOW_R);
	}

	cv::Mat calcChessboardCorners(cv::Size boardSize, float squareSize, cv::Point3f offset)
	{
		Mat corners;

		for (int i = 0; i < boardSize.height; i++)
			for (int j = 0; j < boardSize.width; j++)
				corners.push_back(cv::Point3f(float(j * squareSize), float(i * squareSize), 0) + offset);

		return corners;
	}


	  void infoCallback(const sensor_msgs::CameraInfoConstPtr& info_msg)
	  {
	    cam_model_.fromCameraInfo(info_msg);
	    //pattern_detector_.setCameraMatrices(cam_model_.intrinsicMatrix(), cam_model_.distortionCoeffs());

	    //calibrated = true;
	    //image_sub_ = nh_.subscribe("/camera/rgb/image_mono", 1, &CalibrateKinectCheckerboard::imageCallback, this);

	    ROS_INFO("[calibrate] Got image info!");
	  }
	
	void imageCb(const sensor_msgs::ImageConstPtr& msg)
	{
		geometry_msgs::Pose2D pose_block;
		pose_block.x = 0;
		pose_block.y = 0;
		pose_block.theta = 0;

		Size patternsize(8,6);
		vector<Point2f> board_corners;

		cv_bridge::CvImagePtr cv_ptr_, cv_ptr_2;
		try
		{
			cv_ptr_ = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
			cv_ptr_2 = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
		}
		catch (cv_bridge::Exception& ex)
		{
			ROS_ERROR("cv_bridge exception: %s", ex.what());
			return;
		}

		Mat img_gray_;
		cvtColor(cv_ptr_->image, img_gray_, CV_BGR2GRAY);

		bool patternfound = findChessboardCorners(img_gray_, patternsize, board_corners, CALIB_CB_ADAPTIVE_THRESH + CALIB_CB_NORMALIZE_IMAGE + CALIB_CB_FAST_CHECK);

		drawChessboardCorners(img_gray_, patternsize, cv::Mat(board_corners), patternfound);

		tf::Transform target_transform;
		tf::StampedTransform base_transform;
		try
		{
			ros::Time acquisition_time = msg->header.stamp;
			ros::Duration timeout(1.0 / 30.0);
			if(patternfound)
			{
				
				Eigen::Vector3f translation(0.0, 0.0, 0.0);
				Eigen::Quaternionf orientation(0.0, 0.0, 0.0, 0.0);
				cv::Mat observation_points_ = board_corners;
				cv::Matx33d K;// 3x3の行列で、要素はdouble
				cv::Mat D;// 
				cv::Mat rvec, tvec;//座標を指すベクトル
				
				
				ideal_points_ = calcChessboardCorners(cv::Size(8, 6), 0.025, cv::Point3f(0.0,0.0,0.0));

				//画像上の２次元位置と空間内の３次元位置を対応付ける。
				cv::solvePnP(cv::Mat(ideal_points_), cv::Mat(observation_points_), K, D, rvec, tvec, false);
				translation = Eigen::Vector3f(tvec.at<double>(0, 0), tvec.at<double>(0, 1), tvec.at<double>(0, 2));

				//target_transform.setOrigin(tf::Vector3(translation.x(), translation.y(), translation.z()));
				//target_transform.setRotation(tf::Quaternion(orientation.x(), orientation.y(), orientation.z(), orientation.w()));
				tf_broadcaster_.sendTransform(tf::StampedTransform(target_transform, msg->header.stamp, msg->header.frame_id, target_frame));
			}
		}
		catch (tf::TransformException& ex)
		{
			ROS_WARN("[calibrate] TF exception:\n%s", ex.what());
			return;
		}

		/*
		//Binarization
		Mat img_gray;
		Mat img_bin, img_bin2;
		cvtColor(cv_ptr->image, img_gray, CV_BGR2GRAY);
		medianBlur(img_gray, img_gray, 3);
		threshold(img_gray, img_bin, threshold_bin, 255, THRESH_BINARY);//threshold(img_gray, img_bin, 0, 255, THRESH_BINARY | THRESH_OTSU);

		//Contour Line
		vector<vector<Point> > contours;//輪郭を表現するベクトル
		img_bin2 = img_bin.clone();
		findContours(img_bin2, contours, CV_RETR_EXTERNAL, CV_CHAIN_APPROX_NONE);
		if(contours.empty() != true){
			double max_area=0;
			int max_area_contour=-1;
			for(int i=0;i<contours.size();i++)
			{
				double area=contourArea(contours.at(i));
				if(max_area<area){
					max_area=area;
					max_area_contour=i;
				}
				polylines(cv_ptr->image, contours.at(i), true, Scalar(0, 255, 0), 3);
			}

			//Gravity Point
			int count=contours.at(max_area_contour).size();
			double x=0;
			double y=0;
			for(int i=0;i<count;i++){
				x+=contours.at(max_area_contour).at(i).x;
				y+=contours.at(max_area_contour).at(i).y;
			}
			x/=count;
			y/=count;
			circle(cv_ptr->image, Point(x,y), 10, Scalar(0,0,255), -1, CV_AA);
		}
		 */

		//Segmentation
		/*
		Mat dist;
		Mat kernel1;
		Mat dist_8u;
		Mat markers;
		vector<vector<Point> > contours2;
		distanceTransform(img_bin, dist, CV_DIST_L2, 3);
		normalize(dist, dist, 0, 1., NORM_MINMAX);
		threshold(dist, dist, .4, 1., CV_THRESH_BINARY);
		kernel1 = cv::Mat::ones(3, 3, CV_8UC1);
		dilate(dist, dist, kernel1);
		dist.convertTo(dist_8u, CV_8U);
		findContours(dist_8u, contours2, CV_RETR_EXTERNAL, CV_CHAIN_APPROX_SIMPLE);

		if(!contours2.empty()){
			markers = Mat::zeros(dist.size(), CV_32SC1);
			for (size_t i = 0; i<contours2.size(); i++)
			{
				drawContours(markers, contours2, static_cast<int>(i), Scalar::all(static_cast<int>(i) + 1), -1);
			}
			circle(markers, Point(5, 5), 3, CV_RGB(255, 255, 255), -1);//Background Marker
		}
		 */

		//結果の表示
		imshow(WINDOW_O, cv_ptr_->image);//処理前
		waitKey(10);
		imshow(WINDOW_T, img_gray_);//処理中
		waitKey(10);
		//imshow(WINDOW_R, cv_ptr_2->image);//処理後
		waitKey(10);

		//結果の出力
		image_pub_.publish(cv_ptr_->toImageMsg());
		pose_pub_.publish(pose_block);
	}

};

int main(int argc, char** argv)
{
	ros::init(argc, argv, "block_finder");
	BlockFinder bf;
	ros::spin();
	return 0;
}