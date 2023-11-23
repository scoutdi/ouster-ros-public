/**
 * Copyright (c) 2018-2023, Ouster, Inc.
 * All rights reserved.
 *
 * @file os_driver_nodelet.cpp
 * @brief This node combines the capabilities of os_sensor, os_cloud and os_img
 * into a single ROS nodelet
 */

// prevent clang-format from altering the location of "ouster_ros/os_ros.h", the
// header file needs to be the first include due to PCL_NO_PRECOMPILE flag
// clang-format off
#include "ouster_ros/os_ros.h"
// clang-format on

#include <pluginlib/class_list_macros.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/PointCloud2.h>

#include "os_sensor_nodelet.h"
#include "os_transforms_broadcaster.h"
#include "imu_packet_handler.h"
#include "lidar_packet_handler.h"
#include "point_cloud_processor.h"
#include "laser_scan_processor.h"
#include "image_processor.h"
#include "point_cloud_processor_factory.h"

namespace sensor = ouster::sensor;

namespace ouster_ros {

class OusterDriver : public OusterSensor {
   public:
    OusterDriver() : tf_bcast(getName()) {}
    ~OusterDriver() override {
        NODELET_DEBUG("OusterDriver::~OusterDriver() called");
        halt();
    }

   private:

    virtual void on_metadata_updated(const sensor::sensor_info& info) override {
        OusterSensor::on_metadata_updated(info);

        // for OusterDriver we are going to always assume static broadcast
        // at least for now
        tf_bcast.parse_parameters(getPrivateNodeHandle());
        tf_bcast.broadcast_transforms(info);
    }

    virtual void create_publishers() override {
        auto& pnh = getPrivateNodeHandle();
        auto proc_mask =
            pnh.param("proc_mask", std::string{"IMU|IMG|PCL|SCAN"});
        auto tokens = impl::parse_tokens(proc_mask, '|');

        auto timestamp_mode = pnh.param("timestamp_mode", std::string{});
        double ptp_utc_tai_offset = pnh.param("ptp_utc_tai_offset", -37.0);
        auto min_valid_columns_in_scan = pnh.param("min_valid_columns_in_scan", 0);

        auto& nh = getNodeHandle();

        if (impl::check_token(tokens, "IMU")) {
            imu_pub = nh.advertise<sensor_msgs::Imu>("imu", 100);
            imu_packet_handler = ImuPacketHandler::create_handler(
                info, tf_bcast.imu_frame_id(), timestamp_mode,
                static_cast<int64_t>(ptp_utc_tai_offset * 1e+9));
        }

        int num_returns = get_n_returns(info);

        std::vector<LidarScanProcessor> processors;
        if (impl::check_token(tokens, "PCL")) {
            lidar_pubs.resize(num_returns);
            for (int i = 0; i < num_returns; ++i) {
                lidar_pubs[i] = nh.advertise<sensor_msgs::PointCloud2>(
                    topic_for_return("points", i), 10);
            }

            auto point_type = pnh.param("point_type", std::string{"original"});
            processors.push_back(
                PointCloudProcessorFactory::create_point_cloud_processor(point_type, info,
                    tf_bcast.point_cloud_frame_id(), tf_bcast.apply_lidar_to_sensor_transform(),
                    [this, min_valid_columns_in_scan](PointCloudProcessor_OutputType data) {
                        if (data.num_valid_columns < min_valid_columns_in_scan) {
                            ROS_WARN_STREAM(
                                "Incomplete cloud, dropping it. Got "
                                << data.num_valid_columns << " valid columns, expected "
                                << min_valid_columns_in_scan << ".");
                            return;
                        }
                        for (size_t i = 0; i < data.pc_msgs.size(); ++i)
                            lidar_pubs[i].publish(*data.pc_msgs[i]);
                    }
                )
            );

            // warn about profile incompatibility
            if (PointCloudProcessorFactory::point_type_requires_intensity(point_type) &&
                info.format.udp_profile_lidar == UDPProfileLidar::PROFILE_RNG15_RFL8_NIR8) {
                NODELET_WARN_STREAM(
                    "selected point type '" << point_type << "' is not compatible with the current udp profile: RNG15_RFL8_NIR8");
            }
        }

        if (impl::check_token(tokens, "SCAN")) {
            scan_pubs.resize(num_returns);
            for (int i = 0; i < num_returns; ++i) {
                scan_pubs[i] = nh.advertise<sensor_msgs::LaserScan>(
                    topic_for_return("scan", i), 10);
            }

            // TODO: avoid duplication in os_cloud_node
            int beams_count = static_cast<int>(get_beams_count(info));
            int scan_ring = pnh.param("scan_ring", 0);
            scan_ring = std::min(std::max(scan_ring, 0), beams_count - 1);
            if (scan_ring != pnh.param("scan_ring", 0)) {
                NODELET_WARN_STREAM(
                    "scan ring is set to a value that exceeds available range"
                    "please choose a value between [0, " << beams_count <<
                    "], ring value clamped to: " << scan_ring);
            }

            processors.push_back(LaserScanProcessor::create(
                info, tf_bcast.lidar_frame_id(), scan_ring,
                [this, min_valid_columns_in_scan](LaserScanProcessor::OutputType data) {
                    if (data.num_valid_columns < min_valid_columns_in_scan)
                        return;
                    for (size_t i = 0; i < data.scan_msgs.size(); ++i) {
                        scan_pubs[i].publish(*data.scan_msgs[i]);
                    }
                }));
        }

        if (impl::check_token(tokens, "IMG")) {
            const std::map<sensor::ChanField, std::string>
                channel_field_topic_map_1{
                    {sensor::ChanField::RANGE, "range_image"},
                    {sensor::ChanField::SIGNAL, "signal_image"},
                    {sensor::ChanField::REFLECTIVITY, "reflec_image"},
                    {sensor::ChanField::NEAR_IR, "nearir_image"}};

            const std::map<sensor::ChanField, std::string>
                channel_field_topic_map_2{
                    {sensor::ChanField::RANGE, "range_image"},
                    {sensor::ChanField::SIGNAL, "signal_image"},
                    {sensor::ChanField::REFLECTIVITY, "reflec_image"},
                    {sensor::ChanField::NEAR_IR, "nearir_image"},
                    {sensor::ChanField::RANGE2, "range_image2"},
                    {sensor::ChanField::SIGNAL2, "signal_image2"},
                    {sensor::ChanField::REFLECTIVITY2, "reflec_image2"}};

            auto which_map = num_returns == 1 ? &channel_field_topic_map_1
                                              : &channel_field_topic_map_2;
            for (auto it = which_map->begin(); it != which_map->end(); ++it) {
                image_pubs[it->first] =
                    nh.advertise<sensor_msgs::Image>(it->second, 10);
            }

            processors.push_back(ImageProcessor::create(
                info, tf_bcast.point_cloud_frame_id(),
                [this, min_valid_columns_in_scan](ImageProcessor::OutputType data) {
                    if (data.num_valid_columns < min_valid_columns_in_scan)
                        return;
                    for (auto it = data.image_msgs.begin();
                            it != data.image_msgs.end(); ++it) {
                        image_pubs[it->first].publish(*it->second);
                    }
                }));
        }

        if (impl::check_token(tokens, "PCL") || impl::check_token(tokens, "SCAN") ||
            impl::check_token(tokens, "IMG"))
            lidar_packet_handler = LidarPacketHandler::create_handler(
                info, processors, timestamp_mode,
                static_cast<int64_t>(ptp_utc_tai_offset * 1e+9));
    }

    virtual void on_lidar_packet_msg(const uint8_t* raw_lidar_packet) override {
        if (lidar_packet_handler) lidar_packet_handler(raw_lidar_packet);
    }

    virtual void on_imu_packet_msg(const uint8_t* raw_imu_packet) override {
        if (imu_packet_handler)
            imu_pub.publish(imu_packet_handler(raw_imu_packet));
    }

   private:
    ros::Publisher imu_pub;
    std::vector<ros::Publisher> lidar_pubs;
    std::vector<ros::Publisher> scan_pubs;
    std::map<sensor::ChanField, ros::Publisher> image_pubs;

    OusterTransformsBroadcaster tf_bcast;

    ImuPacketHandler::HandlerType imu_packet_handler;
    LidarPacketHandler::HandlerType lidar_packet_handler;
};

}  // namespace ouster_ros

PLUGINLIB_EXPORT_CLASS(ouster_ros::OusterDriver, nodelet::Nodelet)
