#ifndef PCL_UTIL_HPP
#define PCL_UTIL_HPP

#include <boost/make_shared.hpp>

#include <pcl/PointIndices.h>
#include <pcl/common/common.h>
#include <pcl/common/transforms.h>
#include <pcl/filters/crop_box.h>
#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/io/ply_io.h>
#include <pcl/point_types.h>
#include <pcl/registration/icp.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl_ros/impl/transforms.hpp>
#include <pcl_ros/point_cloud.h>

#include <opencv2/imgproc.hpp>
#include <opencv2/opencv.hpp>

using PCXYZ = pcl::PointCloud<pcl::PointXYZ>;

namespace pcl_util {

/**
 * @brief Load a given ply file in a pcl::POintcloud<pcl::PointXYZ> object.
 * 
 * @param plyPath Path to the ply file.
 * @return PCXYZ::Ptr Pointcloud representation of the ply file.
 */
static PCXYZ::Ptr pcl_from_ply(const std::string &plyPath)
{
  auto plyCloud = boost::make_shared<PCXYZ>();
  ROS_INFO("pcl_from_ply() - Reading from path: %s", plyPath.c_str());
  if (pcl::io::loadPLYFile(plyPath, *plyCloud) == -1) {
    ROS_FATAL("pcl_from_ply() - unable to load whe wall mesh, exiting...");
    throw std::runtime_error("pcl_from_ply() - unable to load wall mesh");
  }
  return plyCloud;
}

/**
 * @brief Perform the ICP algorithm on the given input and target clouds and stores the
 * aligned cloud in the third variable.
 *
 * @param t_inputCloud
 * @param t_targetCloud
 * @param t_alignedCloud
 * @return Eigen::Matrix4f Transformation from the input to the target cloud.
 */
static Eigen::Matrix4f perform_icp(const PCXYZ::Ptr &t_inputCloud,
  const PCXYZ::Ptr &t_targetCloud,
  PCXYZ::Ptr &t_alignedCloud)
{
  constexpr auto WARN_TIMEOUT = 5;
  if (t_inputCloud->empty()) {
    ROS_WARN_THROTTLE(WARN_TIMEOUT, "PatternMatching::do_icp - empty cloud");
    return {};
  }

  pcl::IterativeClosestPoint<pcl::PointXYZ, pcl::PointXYZ> icp;
  icp.setInputSource(t_inputCloud);
  icp.setInputTarget(t_targetCloud);
  t_alignedCloud->clear();
  icp.align(*t_alignedCloud);
  ROS_INFO_COND(icp.hasConverged(),
    "PatternMatching::do_icp - ICP converged. Score: [%.2f]",
    icp.getFitnessScore());
  ROS_FATAL_COND(
    !icp.hasConverged(), "PatternMatching::do_icp - ICP did not converge. :(");

  if (!icp.hasConverged()) { return {}; }

  // Do some stuff if converged
  ROS_INFO_STREAM("ICP transformation: " << icp.getFinalTransformation());
  return icp.getFinalTransformation();
}

/**
 * @brief Perform a Box Filter on the given pointcloud.
 *
 * @param t_inputCloud Input pointcloud pointer.
 * @param t_minHorizontalX
 * @param t_maxHorizontalX
 * @param t_minHorizontalY
 * @param t_maxHorizontalY
 * @param t_minVertical
 * @param t_maxVertical
 * @return PCXYZ::Ptr Filtered pointcloud pointer.
 */
static PCXYZ::Ptr box_filter(const PCXYZ::ConstPtr &t_inputCloud,
  const float t_minHorizontalX,
  const float t_maxHorizontalX,
  const float t_minHorizontalY,
  const float t_maxHorizontalY,
  const float t_minVertical,
  const float t_maxVertical)
{
  if (t_inputCloud->empty()) { return boost::make_shared<PCXYZ>(); }

  auto newCloud = boost::make_shared<PCXYZ>();
  pcl::CropBox<pcl::PointXYZ> boxFilter;
  boxFilter.setMin(
    Eigen::Vector4f(t_minHorizontalX, t_minHorizontalY, t_minVertical, 0.0));
  boxFilter.setMax(
    Eigen::Vector4f(t_maxHorizontalX, t_maxHorizontalY, t_maxVertical, 0.0));
  boxFilter.setInputCloud(t_inputCloud);
  boxFilter.filter(*newCloud);
  return newCloud;
}

/**
 * @brief Upsample the given pointcloud.
 *
 * @param t_inputCloud The input pointcloud pointer.
 * @param t_scalingFactor
 * @param t_upsampleIncrement
 * @param t_upsampleOffset
 * @param t_upsampleIter
 * @return PCXYZ::Ptr Upsampled pointcloud.
 */
static PCXYZ::Ptr upsample_pointcloud(const PCXYZ::Ptr &t_inputCloud,
  const float t_scalingFactor,
  const float t_upsampleIncrement,
  const float t_upsampleOffset,
  const int t_upsampleIter)
{
  // Add the initial pointcloud pattern
  auto upscaledPointcloud = boost::make_shared<PCXYZ>();
  for (const auto &point : t_inputCloud->points) {
    upscaledPointcloud->points.emplace_back(pcl::PointXYZ(
      point.x / t_scalingFactor, point.y / t_scalingFactor, point.z / t_scalingFactor));
  }

  // Upsample the pointcloud
  const auto upsample_element = [&](const float unscaledPoint, const int iter) {
    return unscaledPoint / t_scalingFactor + t_upsampleOffset
           + iter * t_upsampleIncrement;
  };
  for (int i = 0; i < t_upsampleIter; i++) {
    for (int j = 0; j < t_upsampleIter; j++) {
      for (const auto &point : t_inputCloud->points) {
        upscaledPointcloud->points.emplace_back(
          pcl::PointXYZ(upsample_element(point.x, i),
            upsample_element(point.y, j),
            point.z / t_scalingFactor));
      }
    }
  }
  return upscaledPointcloud;
}

/**
 * @brief Perform outlier filtering on the given pointcloud
 *
 * @param t_inputCloud Input pointcloud.
 * @param t_filterMean Number of nearest neighbours of any point.
 * @param t_filterStddev Standard deviation multiplier for the filtering.
 * @return PCXYZ::Ptr Filtered pointcloud.
 */
static PCXYZ::Ptr do_outlier_filtering(const PCXYZ::Ptr &t_inputCloud,
  const int t_filterMean,
  const double t_filterStddev)
{
  auto filteredCloud = boost::make_shared<PCXYZ>();
  pcl::StatisticalOutlierRemoval<pcl::PointXYZ> sor;
  sor.setInputCloud(t_inputCloud);
  sor.setMeanK(t_filterMean);
  sor.setStddevMulThresh(t_filterStddev);
  sor.filter(*filteredCloud);
  return filteredCloud;
}

/**
 * @brief Demean the pointcloud wrt. the given cloud centroid.
 *
 * @param t_cloudWithMean Input pointcloud pointer.
 * @param t_cloudCentroid Input centroid.
 * @return PCXYZ::Ptr Demeaned pointcloud.
 */
static PCXYZ::Ptr demean_pointcloud(const PCXYZ::Ptr &t_cloudWithMean,
  const Eigen::Vector4f &t_cloudCentroid)
{
  if (t_cloudWithMean->empty()) { return t_cloudWithMean; }

  auto outCloud = boost::make_shared<PCXYZ>();
  // pcl::compute3DCentroid(*t_cloudWithMean, m_lastFoundMapCentroid); TODO: Add this line
  // before
  for (const auto &point : t_cloudWithMean->points) {
    outCloud->points.emplace_back(pcl::PointXYZ(point.x - t_cloudCentroid.x(),
      point.y - t_cloudCentroid.y(),
      point.z - t_cloudCentroid.z()));
  }
  return outCloud;
}

/**
 * @brief Organize the given pointcloud.
 *
 * @param t_unorganizedCloud Unorganized pointcloud input.
 * @param t_width Width of the organized cloud.
 * @param t_height Height of the organized cloud.
 * @param t_resolution Resolution of the organized cloud.
 * @return PCXYZ::Ptr Returns pointer to the organized pointcloud.
 */
static PCXYZ::Ptr organize_pointcloud(const PCXYZ::Ptr &t_unorganizedCloud,
  const double t_resolution = 20,
  const int t_width = 100,
  const int t_height = 100)
{
  if (t_unorganizedCloud->empty()) { return t_unorganizedCloud; }

  auto organizedCloud = boost::make_shared<PCXYZ>(
    t_width * t_resolution, t_height * t_resolution, pcl::PointXYZ(0, 0, 0));

  const auto out_of_range = [&organizedCloud](
                              const std::size_t t_x, const std::size_t t_y) {
    return t_x >= organizedCloud->width || t_y >= organizedCloud->height;
  };
  const auto is_point_empty = [](const pcl::PointXYZ &point) {
    return point.x == 0 && point.y == 0 && point.z == 0;
  };
  const std::size_t indexOffsetX = round(organizedCloud->width / 2.0);
  const std::size_t indexOffsetY = round(organizedCloud->height / 2.0);
  const auto add_to_organized_cloud = [&](const pcl::PointXYZ &point) {
    const std::size_t indX = round(point.x * t_resolution) + indexOffsetX;
    const std::size_t indY = round(point.y * t_resolution) + indexOffsetY;

    if (out_of_range(indX, indY)) { return; }
    auto currentPoint = organizedCloud->at(indX, indY);
    if (is_point_empty(currentPoint) || point.z > currentPoint.z) {
      organizedCloud->at(indX, indY) = point;
    }
  };

  std::for_each(t_unorganizedCloud->points.begin(),
    t_unorganizedCloud->points.end(),
    add_to_organized_cloud);

  return organizedCloud;
}

/**
 * @brief Transform the input organized pointcloud to a cv::Mat type.
 *
 * @param t_inputCloud Input pointcloud.
 * @return cv::Mat Point cloud Matrix, empty if conversion unsuccessful.
 */
static cv::Mat PCXYZ_to_cvMat(const PCXYZ::Ptr &t_inputCloud)
{
  if (t_inputCloud->empty()) {
    ROS_INFO("PCXYZ_to_cvMAT - input cloud is empty");
    return cv::Mat();
  }

  if (!t_inputCloud->isOrganized()) {
    ROS_FATAL("PXCYZ_to_cvMAT - cannot convert an unorganized pointcloud");
    return cv::Mat();
  }

  ROS_INFO_COND(
    t_inputCloud->isOrganized(), "WallDetection::PCXYZ_to_cvMat - cloud is organized");
  ROS_INFO("Size: [%d, %d]", t_inputCloud->height, t_inputCloud->width);

  static constexpr auto WHITE_VAL = 255;
  cv::Mat outImage(t_inputCloud->height, t_inputCloud->width, CV_8UC1, cv::Scalar(0));
  for (int x = 0; x < outImage.rows; x++) {
    for (int y = 0; y < outImage.cols; y++) {
      auto point = t_inputCloud->at(x, y);
      if (point.x == 0 && point.y == 0 && point.z == 0) { continue; }
      outImage.at<uchar>(x, y) = WHITE_VAL;
    }
  }
  return outImage;
}
}// namespace pcl_util

#endif