/**
 * @copyright 2020, 2021 Istituto Italiano di Tecnologia (IIT). This software may be modified and
 * distributed under the terms of the BSD-3-Clause license.
 */

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>

#include <BipedalLocomotion/ParametersHandler/IParametersHandler.h>
#include <BipedalLocomotion/ParametersHandler/StdImplementation.h>
#include <BipedalLocomotion/ParametersHandler/YarpImplementation.h>
#include <BipedalLocomotion/System/Clock.h>
#include <BipedalLocomotion/System/YarpClock.h>
#include <BipedalLocomotion/TextLogging/Logger.h>
#include <BipedalLocomotion/TextLogging/LoggerBuilder.h>
#include <BipedalLocomotion/TextLogging/YarpLogger.h>
#include <BipedalLocomotion/YarpRobotLoggerDevice.h>
#include <BipedalLocomotion/YarpRobotLoggerDeviceVariableTreeNames.h>
#include <BipedalLocomotion/YarpTextLoggingUtilities.h>
#include <BipedalLocomotion/YarpUtilities/Helper.h>
#include <BipedalLocomotion/YarpUtilities/VectorsCollection.h>

#include <yarp/eigen/Eigen.h>
#include <yarp/os/BufferedPort.h>
#include <yarp/profiler/NetworkProfiler.h>

#include <robometry/BufferConfig.h>
#include <robometry/BufferManager.h>

#include <process.hpp>

using namespace BipedalLocomotion::YarpUtilities;
using namespace BipedalLocomotion::ParametersHandler;
using namespace BipedalLocomotion::RobotInterface;
using namespace BipedalLocomotion;

VISITABLE_STRUCT(TextLoggingEntry,
                 level,
                 text,
                 filename,
                 line,
                 function,
                 hostname,
                 cmd,
                 args,
                 pid,
                 thread_id,
                 component,
                 id,
                 systemtime,
                 networktime,
                 externaltime,
                 backtrace,
                 yarprun_timestamp,
                 local_timestamp);

void findAndReplaceAll(std::string& data, const std::string& toSearch, const std::string& replaceStr)
{
    // Get the first occurrence
    size_t pos = data.find(toSearch);
    // Repeat till end is reached
    while (pos != std::string::npos)
    {
        // Replace this occurrence of Sub String
        data.replace(pos, toSearch.size(), replaceStr);
        // Get the next occurrence from the current position
        pos = data.find(toSearch, pos + replaceStr.size());
    }
}

bool YarpRobotLoggerDevice::VectorsCollectionSignal::connect()
{
    return client.connect();
}

void YarpRobotLoggerDevice::VectorsCollectionSignal::disconnect()
{
    if (connected)
    {
        client.disconnect();
    }
}

YarpRobotLoggerDevice::YarpRobotLoggerDevice(double period,
                                             yarp::os::ShouldUseSystemClock useSystemClock)
    : yarp::os::PeriodicThread(period, useSystemClock)
{
    // Use the yarp clock in blf
    BipedalLocomotion::System::ClockBuilder::setFactory(
        std::make_shared<BipedalLocomotion::System::YarpClockFactory>());

    // the logging message are streamed using yarp
    BipedalLocomotion::TextLogging::LoggerBuilder::setFactory(
        std::make_shared<BipedalLocomotion::TextLogging::YarpLoggerFactory>());

}

YarpRobotLoggerDevice::YarpRobotLoggerDevice()
    : yarp::os::PeriodicThread(0.01, yarp::os::ShouldUseSystemClock::No)
{
    // Use the yarp clock in blf
    BipedalLocomotion::System::ClockBuilder::setFactory(
        std::make_shared<BipedalLocomotion::System::YarpClockFactory>());

    // the logging message are streamed using yarp
    BipedalLocomotion::TextLogging::LoggerBuilder::setFactory(
        std::make_shared<BipedalLocomotion::TextLogging::YarpLoggerFactory>());

}

YarpRobotLoggerDevice::~YarpRobotLoggerDevice() = default;

bool YarpRobotLoggerDevice::open(yarp::os::Searchable& config)
{

    constexpr auto logPrefix = "[YarpRobotLoggerDevice::open]";
    auto params = std::make_shared<ParametersHandler::YarpImplementation>(config);

    double devicePeriod{0.01};
    if (params->getParameter("sampling_period_in_s", devicePeriod))
    {
        this->setPeriod(devicePeriod);
    }

    if (!params->getParameter("text_logging_subnames", m_textLoggingSubnames))
    {
        log()->info("{} Unable to get the 'text_logging_subnames' parameter for the telemetry. All "
                    "the ports related to the text logging will be considered.",
                    logPrefix);
    }

    if (!params->getParameter("code_status_cmd_prefixes", m_codeStatusCmdPrefixes))
    {
        log()->info("{} Unable to get the 'code_status_cmd_prefixes' parameter. No prefix will be "
                    "added to commands.",
                    logPrefix);
    }

    if (!this->setupRobotSensorBridge(params->getGroup("RobotSensorBridge")))
    {
        return false;
    }

    if (this->setupRobotCameraBridge(params->getGroup("RobotCameraBridge")))
    {
        auto populateCamerasData
            = [logPrefix, params, this](const std::string& fpsParamName,
                                        const std::vector<std::string>& cameraNames) -> bool {
            std::vector<int> fps, depthScale;
            std::vector<std::string> rgbSaveMode, depthSaveMode;
            if (!params->getParameter(fpsParamName, fps))
            {
                log()->error("{} Unable to find the parameter named: {}.", logPrefix, fpsParamName);
                return false;
            }

            // if the camera is an rgbd camera then user should provide the depth scale
            if ((&cameraNames) == (&m_cameraBridge->getMetaData().sensorsList.rgbdCamerasList))
            {
                if (!params->getParameter("rgbd_cameras_depth_scale", depthScale))
                {
                    log()->error("{} Unable to find the parameter named: "
                                 "'rgbd_cameras_depth_scale'.",
                                 logPrefix);
                    return false;
                }

                if (!params->getParameter("rgbd_cameras_rgb_save_mode", rgbSaveMode))
                {
                    log()->error("{} Unable to find the parameter named: "
                                 "'rgb_cameras_rgb_save_mode.",
                                 logPrefix);
                    return false;
                }

                if (!params->getParameter("rgbd_cameras_depth_save_mode", depthSaveMode))
                {
                    log()->error("{} Unable to find the parameter named: "
                                 "'rgbd_cameras_depth_save_mode.",
                                 logPrefix);
                    return false;
                }

                if (fps.size() != depthScale.size() || (fps.size() != rgbSaveMode.size())
                    || (fps.size() != depthSaveMode.size()))
                {
                    log()->error("{} Mismatch between the vector containing the size of the vector "
                                 "provided from configuration"
                                 "Number of cameras: {}. Size of the FPS vector {}. Size of the "
                                 "depth scale vector {}."
                                 "Size of 'rgb_cameras_rgb_save_mode' {}. Size of "
                                 "'rgb_cameras_depth_save_mode': {}",
                                 logPrefix,
                                 cameraNames.size(),
                                 fps.size(),
                                 depthScale.size(),
                                 rgbSaveMode.size(),
                                 depthSaveMode.size());
                    return false;
                }
            } else
            {
                if (!params->getParameter("rgb_cameras_rgb_save_mode", rgbSaveMode))
                {
                    log()->error("{} Unable to find the parameter named: "
                                 "'rgb_cameras_rgb_save_mode.",
                                 logPrefix);
                    return false;
                }
            }

            if ((fps.size() != rgbSaveMode.size()))
            {
                log()->error("{} Mismatch between the vector containing the size of the vector "
                             "provided from configuration"
                             "Number of cameras: {}. Size of the FPS vector {}."
                             "Size of 'rgb_cameras_rgb_save_mode' {}.",
                             logPrefix,
                             cameraNames.size(),
                             fps.size(),
                             rgbSaveMode.size());
                return false;
            }

            if (fps.size() != cameraNames.size())
            {
                log()->error("{} Mismatch between the number of cameras and the vector containing "
                             "the FPS. Number of cameras: {}. Size of the FPS vector {}.",
                             logPrefix,
                             cameraNames.size(),
                             fps.size());
                return false;
            }

            auto createImageSaver
                = [logPrefix](
                      const std::string& saveMode) -> std::shared_ptr<VideoWriter::ImageSaver> {
                auto saver = std::make_shared<VideoWriter::ImageSaver>();
                if (saveMode == "frame")
                {
                    saver->saveMode = VideoWriter::SaveMode::Frame;
                } else if (saveMode == "video")
                {
                    saver->saveMode = VideoWriter::SaveMode::Video;
                } else
                {
                    log()->error("{} The save mode associated to the one of the camera is neither "
                                 "'frame' nor 'video'. Provided: {}",
                                 logPrefix,
                                 saveMode);
                    return nullptr;
                }
                return saver;
            };

            bool ok = true;
            for (unsigned int i = 0; i < fps.size(); i++)
            {
                if (fps[i] <= 0)
                {
                    log()->error("{} The FPS associated to the camera {} is negative or equal to "
                                 "zero.",
                                 logPrefix,
                                 i);
                    return false;
                }

                // get the desired fps for each camera
                m_videoWriters[cameraNames[i]].fps = fps[i];

                // this means that the list of cameras are rgb camera
                if ((&cameraNames) == (&m_cameraBridge->getMetaData().sensorsList.rgbCamerasList))
                {
                    m_videoWriters[cameraNames[i]].rgb = createImageSaver(rgbSaveMode[i]);
                    ok = ok && m_videoWriters[cameraNames[i]].rgb != nullptr;
                }
                // this means that the list of cameras are rgbd camera
                else
                {
                    if (depthSaveMode[i] == "video")
                    {
                        log()->warn("{} The depth stream of the rgbd camera {} will be saved as a "
                                    "grayscale 8bit video. We suggest to save it as a set of "
                                    "frames.",
                                    logPrefix,
                                    i);
                    }
                    m_videoWriters[cameraNames[i]].rgb = createImageSaver(rgbSaveMode[i]);
                    ok = ok && m_videoWriters[cameraNames[i]].rgb != nullptr;
                    m_videoWriters[cameraNames[i]].depth = createImageSaver(depthSaveMode[i]);
                    ok = ok && m_videoWriters[cameraNames[i]].depth != nullptr;
                    m_videoWriters[cameraNames[i]].depthScale = depthScale[i];
                }
            }

            return ok;
        };

        // get the metadata for rgb camera
        if (m_cameraBridge->getMetaData().bridgeOptions.isRGBCameraEnabled)
        {
            if (!populateCamerasData("rgb_cameras_fps",
                                     m_cameraBridge->getMetaData().sensorsList.rgbCamerasList))
            {
                log()->error("{} Unable to populate the camera fps for RGB cameras.", logPrefix);
                return false;
            }
        }

        // Currently the logger supports only rgb cameras
        if (m_cameraBridge->getMetaData().bridgeOptions.isRGBDCameraEnabled)
        {
            if (!populateCamerasData("rgbd_cameras_fps",
                                     m_cameraBridge->getMetaData().sensorsList.rgbdCamerasList))
            {
                log()->error("{} Unable to populate the camera fps for RGBD cameras.", logPrefix);
                return false;
            }
        }

        // get the video codec in case rgb or rgbd camera are enabled
        if (m_cameraBridge->getMetaData().bridgeOptions.isRGBDCameraEnabled
            || m_cameraBridge->getMetaData().bridgeOptions.isRGBCameraEnabled)
        {
            if (!params->getParameter("video_codec_code", m_videoCodecCode))
            {
                constexpr auto fourccCodecUrl = "https://abcavi.kibi.ru/fourcc.php";
                log()->info("{} The parameter 'video_codec_code' is not provided. The default one "
                            "will be used {}. You can find the list of supported parameters at: "
                            "{}.",
                            logPrefix,
                            m_videoCodecCode,
                            fourccCodecUrl);
            } else if (m_videoCodecCode.size() != 4)
            {
                constexpr auto fourccCodecUrl = "https://abcavi.kibi.ru/fourcc.php";
                log()->error("{} The parameter 'video_codec_code' must be a string with 4 "
                             "characters. You can find the list of supported parameters at: {}.",
                             logPrefix,
                             fourccCodecUrl);
                return false;
            }
        }
    } else
    {
        log()->info("{} The video will not be recorded", logPrefix);
    }

    if (!this->setupTelemetry(params->getGroup("Telemetry"), devicePeriod))
    {
        return false;
    }

    if (!this->setupExogenousInputs(params->getGroup("ExogenousSignals")))
    {
        return false;
    }

    return true;
}

bool YarpRobotLoggerDevice::setupExogenousInputs(
    std::weak_ptr<const ParametersHandler::IParametersHandler> params)
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::setupExogenousInputs]";

    auto ptr = params.lock();
    if (ptr == nullptr)
    {
        log()->info("{} No exogenous input will be logged.", logPrefix);
        return true;
    }

    std::vector<std::string> inputs;
    if (!ptr->getParameter("vectors_collection_exogenous_inputs", inputs))
    {
        log()->error("{} Unable to get the exogenous inputs.", logPrefix);
        return false;
    }

    for (const auto& input : inputs)
    {
        auto group = ptr->getGroup(input).lock();
        std::string signalName, remote;
        if (group == nullptr)
        {
            log()->error("{} Unable to get the group named {}.", logPrefix, input);
            return false;
        }

        if (!group->getParameter("remote", remote))
        {
            log()->error("{} Unable to get the remote parameter for the group named {}.",
                         logPrefix,
                         input);
            return false;
        }

        if (!group->getParameter("signal_name", signalName))
        {
            log()->error("{} Unable to get the signal_name parameter for the group named {}.",
                         logPrefix,
                         input);
            return false;
        }

        m_vectorsCollectionSignals[remote].signalName = signalName;
        if (!m_vectorsCollectionSignals[remote].client.initialize(group))
        {
            log()->error("{} Unable to initialize the vectors collection signal for the group "
                         "named {}.",
                         logPrefix,
                         signalName);
            return false;
        }
    }

    if (!ptr->getParameter("vectors_exogenous_inputs", inputs))
    {
        log()->error("{} Unable to get the exogenous inputs.", logPrefix);
        return false;
    }

    for (const auto& input : inputs)
    {
        auto group = ptr->getGroup(input).lock();
        std::string local, signalName, remote, carrier;
        if (group == nullptr || !group->getParameter("local", local)
            || !group->getParameter("remote", remote) || !group->getParameter("carrier", carrier)
            || !group->getParameter("signal_name", signalName))
        {
            log()->error("{} Unable to get the parameters related to the input: {}.",
                         logPrefix,
                         input);
            return false;
        }

        m_vectorSignals[remote].signalName = signalName;
        m_vectorSignals[remote].remote = remote;
        m_vectorSignals[remote].local = local;
        m_vectorSignals[remote].carrier = carrier;

        if (!m_vectorSignals[remote].port.open(m_vectorSignals[remote].local))
        {
            log()->error("{} Unable to open the port named: {}.",
                         logPrefix,
                         m_vectorSignals[remote].local);
            return false;
        }
    }

    return true;
}

bool YarpRobotLoggerDevice::setupTelemetry(
    std::weak_ptr<const ParametersHandler::IParametersHandler> params, const double& devicePeriod)
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::setupTelemetry]";

    auto ptr = params.lock();
    if (ptr == nullptr)
    {
        log()->error("{} The parameters handler is not valid.", logPrefix);
        return false;
    }

    robometry::BufferConfig config;
    char* tmp = std::getenv("YARP_ROBOT_NAME");
    // if the variable does not exist it points to NULL
    if (tmp != NULL)
    {
        config.yarp_robot_name = tmp;
    }
    config.filename = "robot_logger_device";
    config.auto_save = true;
    config.save_periodically = true;
    config.file_indexing = "%Y_%m_%d_%H_%M_%S";
    config.mat_file_version = matioCpp::FileVersion::MAT7_3;

    if (!ptr->getParameter("save_period", config.save_period))
    {
        log()->error("{} Unable to get the 'save_period' parameter for the telemetry.", logPrefix);
        return false;
    }

    // the telemetry will flush the content of its storage every config.save_period
    // and this device runs every devicePeriod
    // so the size of the telemetry buffer must be at least config.save_period / devicePeriod
    // to be sure we are not going to lose data the buffer will be 10% longer
    constexpr double percentage = 0.1;
    config.n_samples = static_cast<int>(std::ceil((1 + percentage) //
                                                  * (config.save_period / devicePeriod)));

    return m_bufferManager.configure(config);
}

bool YarpRobotLoggerDevice::setupRobotSensorBridge(
    std::weak_ptr<const ParametersHandler::IParametersHandler> params)
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::setupRobotSensorBridge]";

    auto ptr = params.lock();
    if (ptr == nullptr)
    {
        log()->error("{} The parameters handler is not valid.", logPrefix);
        return false;
    }

    m_robotSensorBridge = std::make_unique<YarpSensorBridge>();
    if (!m_robotSensorBridge->initialize(ptr))
    {
        log()->error("{} Unable to configure the 'SensorBridge'", logPrefix);
        return false;
    }

    // Get additional flags required by the device
    if (!ptr->getParameter("stream_joint_states", m_streamJointStates))
    {
        log()->info("{} The 'stream_joint_states' parameter is not found. The joint states is not "
                    "logged",
                    logPrefix);
    }

    if (!ptr->getParameter("stream_motor_states", m_streamMotorStates))
    {
        log()->info("{} The 'stream_motor_states' parameter is not found. The motor states is not "
                    "logged",
                    logPrefix);
    }

    if (!ptr->getParameter("stream_motor_PWM", m_streamMotorPWM))
    {
        log()->info("{} The 'stream_motor_PWM' parameter is not found. The motor PWM is not logged",
                    logPrefix);
    }

    if (!ptr->getParameter("stream_pids", m_streamPIDs))
    {
        log()->info("{} The 'stream_pids' parameter is not found. The motor pid values are not "
                    "logged",
                    logPrefix);
    }

    if (!ptr->getParameter("stream_inertials", m_streamInertials))
    {
        log()->info("{} The 'stream_inertials' parameter is not found. The IMU values are not "
                    "logged",
                    logPrefix);
    }

    if (!ptr->getParameter("stream_cartesian_wrenches", m_streamCartesianWrenches))
    {
        log()->info("{} The 'stream_cartesian_wrenches' parameter is not found. The cartesian "
                    "wrench values are not "
                    "logged",
                    logPrefix);
    }

    if (!ptr->getParameter("stream_forcetorque_sensors", m_streamFTSensors))
    {
        log()->info("{} The 'stream_forcetorque_sensors' parameter is not found. The FT values are "
                    "not "
                    "logged",
                    logPrefix);
    }

    if (!ptr->getParameter("stream_temperatures", m_streamTemperatureSensors))
    {
        log()->info("{} The 'stream_temperatures' parameter is not found. The temperature sensor "
                    "values are not "
                    "logged",
                    logPrefix);
    }

    return true;
}

bool YarpRobotLoggerDevice::setupRobotCameraBridge(
    std::weak_ptr<const ParametersHandler::IParametersHandler> params)
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::setupRobotCameraBridge]";

    auto ptr = params.lock();
    if (ptr == nullptr)
    {
        log()->error("{} The parameters handler is not valid.", logPrefix);
        return false;
    }

    m_cameraBridge = std::make_unique<YarpCameraBridge>();
    if (!m_cameraBridge->initialize(ptr))
    {
        log()->error("{} Unable to configure the 'Camera bridge'", logPrefix);
        return false;
    }

    return true;
}

bool YarpRobotLoggerDevice::addJointRelatedChannelAndMetadata(const std::string& name, const std::vector<std::string>& jointNames)
{
    bool ok = m_bufferManager.addChannel({name, {jointNames.size(), 1}, jointNames});
    std::string rtMetadataName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + name;
    ok &= m_vectorCollectionRTDataServer.populateMetadata(rtMetadataName, jointNames);
    return ok;
}

bool YarpRobotLoggerDevice::addSensorBridgeRelatedChannelAndMetadata(const std::string& nameKey, const std::vector<std::string>& metadataNames)
{
    bool ok = m_bufferManager.addChannel({nameKey, {metadataNames.size(), 1}, metadataNames});
    std::string rtNameKey = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + nameKey;
    ok &= m_vectorCollectionRTDataServer.populateMetadata(rtNameKey, metadataNames);
    return ok;
}

bool YarpRobotLoggerDevice::attachAll(const yarp::dev::PolyDriverList& poly)
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::attachAll]";

    bool ok = true;
    // open the TextLogging port
    ok = ok && m_textLoggingPort.open(m_textLoggingPortName);
    // run the thread
    m_lookForNewLogsThread = std::thread([this] { this->lookForNewLogs(); });

    // run the thread for reading the exogenous signals
    m_lookForNewExogenousSignalThread = std::thread([this] { this->lookForExogenousSignals(); });

    if (!m_robotSensorBridge->setDriversList(poly))
    {
        log()->error("{} Could not attach drivers list to sensor bridge.", logPrefix);
        return false;
    }

    // The user can avoid to record the camera
    if (m_cameraBridge != nullptr)
    {
        if (!m_cameraBridge->setDriversList(poly))
        {
            log()->error("{} Could not attach drivers list to camera bridge.", logPrefix);
            return false;
        }
    }

    // TODO this should be removed
    // this sleep is required since the sensor bridge could be not ready
    using namespace std::chrono_literals;
    BipedalLocomotion::clock().sleepFor(2000ms);

    std::vector<std::string> joints;
    if (!m_robotSensorBridge->getJointsList(joints))
    {
        log()->error("{} Could not get the joints list.", logPrefix);
        return false;
    }

    // initialize the RT Logger Vector Collection Server
    auto loggerOption = std::make_shared<BipedalLocomotion::ParametersHandler::YarpImplementation>();
    loggerOption->setParameter("remote", "/testVectorCollections");
    if (!m_vectorCollectionRTDataServer.initialize(loggerOption))
    {
        log()->error("Failed to initalize the vectorsCollectionServer", logPrefix);
        return false;
    }
    char* tmp = std::getenv("YARP_ROBOT_NAME");
    std::string metadataName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(YARP_NAME);
    m_vectorCollectionRTDataServer.populateMetadata(metadataName, {std::string(tmp)});

    metadataName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(TIMESTAMPS_NAME);
    m_vectorCollectionRTDataServer.populateMetadata(metadataName, {TIMESTAMPS_NAME});


    const unsigned dofs = joints.size();
    m_bufferManager.setDescriptionList(joints);
    std::string rtMetadataName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(ROBOT_DESCRIPTON_LIST);
    m_vectorCollectionRTDataServer.populateMetadata(rtMetadataName, joints);


    // prepare the telemetry
    if (m_streamJointStates)
    {
        ok &= addJointRelatedChannelAndMetadata(JOINT_STATE_POSITIONS_NAME, joints);
        ok &= addJointRelatedChannelAndMetadata(JOINT_STATE_VELOCITIES_NAME, joints);
        ok &= addJointRelatedChannelAndMetadata(JOINT_STATE_ACCLERATIONS_NAME, joints);
        ok &= addJointRelatedChannelAndMetadata(JOINT_STATE_TORQUES_NAME, joints);
    }
    if (m_streamMotorStates)
    {
        ok &= addJointRelatedChannelAndMetadata(MOTOR_STATE_POSITIONS_NAME, joints);
        ok &= addJointRelatedChannelAndMetadata(MOTOR_STATE_VELOCITIES_NAME, joints);
        ok &= addJointRelatedChannelAndMetadata(MOTOR_STATE_ACCELERATIONS_NAME, joints);
        ok &= addJointRelatedChannelAndMetadata(MOTOR_STATE_CURRENTS_NAME, joints);
    }

    if (m_streamMotorPWM)
    {
        ok &= addJointRelatedChannelAndMetadata(MOTOR_STATE_PWM_NAME, joints);
    }

    if (m_streamPIDs)
    {
        ok &= addJointRelatedChannelAndMetadata(MOTOR_STATE_PIDS_NAME, joints);
    }

    if (m_streamFTSensors)
    {
        for (const auto& sensorName : m_robotSensorBridge->getSixAxisForceTorqueSensorsList())
        {
            std::string fullFTSensorName = std::string(FTS_NAME) + std::string(TREE_DELIM) + sensorName;
            ok &= addSensorBridgeRelatedChannelAndMetadata(fullFTSensorName, FTElementNames);
        }
    }

    if (m_streamInertials)
    {
        for (const auto& sensorName : m_robotSensorBridge->getGyroscopesList())
        {
            std::string fullGyroSensorName = std::string(GYROS_NAME) + std::string(TREE_DELIM) + sensorName;
            ok &= addSensorBridgeRelatedChannelAndMetadata(fullGyroSensorName, GyroElementNames);
        }

        for (const auto& sensorName : m_robotSensorBridge->getLinearAccelerometersList())
        {
            std::string fullAccelerometerSensorName = std::string(ACCELEROMETERS_NAME) + std::string(TREE_DELIM) + sensorName;
            ok &= addSensorBridgeRelatedChannelAndMetadata(fullAccelerometerSensorName, AccelerometerElementNames);
        }

        for (const auto& sensorName : m_robotSensorBridge->getOrientationSensorsList())
        {
            std::string fullOrientationsSensorName = std::string(ORIENTATIONS_NAME) + std::string(TREE_DELIM) + sensorName;
            ok &= addSensorBridgeRelatedChannelAndMetadata(fullOrientationsSensorName, OrientationElementNames);
        }

        for (const auto& sensorName : m_robotSensorBridge->getMagnetometersList())
        {
            std::string fullMagnetometersSensorName = std::string(MAGNETOMETERS_NAME) + std::string(TREE_DELIM) + sensorName;
            ok &= addSensorBridgeRelatedChannelAndMetadata(fullMagnetometersSensorName, MagnetometerElementNames);
        }

        // an IMU contains a gyro accelerometer and an orientation sensor
        for (const auto& sensorName : m_robotSensorBridge->getIMUsList())
        {
            std::string fullAccelerometerSensorName = std::string(ACCELEROMETERS_NAME) + std::string(TREE_DELIM) + sensorName;
            std::string fullGyroSensorName = std::string(GYROS_NAME) + std::string(TREE_DELIM) + sensorName;
            std::string fullOrientationsSensorName = std::string(ORIENTATIONS_NAME) + std::string(TREE_DELIM) + sensorName;
            ok &= addSensorBridgeRelatedChannelAndMetadata(fullAccelerometerSensorName, AccelerometerElementNames);
            ok &= addSensorBridgeRelatedChannelAndMetadata(fullGyroSensorName, GyroElementNames);
            ok &= addSensorBridgeRelatedChannelAndMetadata(fullOrientationsSensorName, OrientationElementNames);
        }
    }

    if (m_streamCartesianWrenches)
    {
        for (const auto& cartesianWrenchName : m_robotSensorBridge->getCartesianWrenchesList())
        {
            std::string fullCartesianWrenchName = std::string(CARTESIAN_WRENCHES_NAME) + std::string(TREE_DELIM) + cartesianWrenchName;
            ok &= addSensorBridgeRelatedChannelAndMetadata(fullCartesianWrenchName, CartesianWrenchNames);
        }
    }

    if (m_streamTemperatureSensors)
    {
        for (const auto& sensorName : m_robotSensorBridge->getTemperatureSensorsList())
        {
            std::string fullTemperatureSensorName = std::string(TEMPERATURE_NAME) + std::string(TREE_DELIM) + sensorName;
            ok &= addSensorBridgeRelatedChannelAndMetadata(fullTemperatureSensorName, TemperatureNames);
        }
    }
    std::string signalName = "";
    std::string rtSignalName = "";
    for (auto& [name, signal] : m_vectorsCollectionSignals)
    {
        std::lock_guard<std::mutex> lock(signal.mutex);
        BipedalLocomotion::YarpUtilities::VectorsCollection* externalSignalCollection = signal.client.readData(false);
        if (externalSignalCollection != nullptr)
        {
            for (const auto& [key, vector] : externalSignalCollection->vectors)
            {
                signalName = signal.signalName + std::string(TREE_DELIM) + key;
                const auto& metadata = signal.metadata.vectors.find(key);
                if (metadata == signal.metadata.vectors.cend())
                {
                    log()->warn("{} Unable to find the metadata for the signal named {}. The "
                                "default one will be used.",
                                logPrefix,
                                signalName);
                    m_bufferManager.addChannel({signalName, {vector.size(), 1}});
                    rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + signalName;

                    m_vectorCollectionRTDataServer.populateMetadata(rtSignalName, {});
                } else
                {
                    // if the metadata is found we use it
                    m_bufferManager.addChannel({signalName, //
                                                {vector.size(), 1},
                                                metadata->second});
                    rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + signalName;
                    m_vectorCollectionRTDataServer.populateMetadata(rtSignalName, {metadata->second});
                }
            }
        }
    }

    for (auto& [name, signal] : m_vectorSignals)
    {
        std::lock_guard<std::mutex> lock(signal.mutex);
        yarp::sig::Vector* vector = signal.port.read(false);
        if (vector != nullptr)
        {
            if (!signal.dataArrived)
            {
                m_bufferManager.addChannel({signal.signalName, {vector->size(), 1}});

                rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + signal.signalName;
                m_vectorCollectionRTDataServer.populateMetadata(rtSignalName, {});
                signal.dataArrived = true;
            }
        }
    }

    m_vectorCollectionRTDataServer.finalizeMetadata();

    // resize the temporary vectors
    m_jointSensorBuffer.resize(dofs);


    // The user can avoid to record the camera
    if (m_cameraBridge != nullptr)
    {
        ok = ok && m_cameraBridge->getRGBCamerasList(m_rgbCamerasList);
        for (const auto& camera : m_rgbCamerasList)
        {
            if (m_videoWriters[camera].rgb->saveMode == VideoWriter::SaveMode::Video)
            {
                if (!this->openVideoWriter(m_videoWriters[camera].rgb,
                                           camera,
                                           "rgb",
                                           m_cameraBridge->getMetaData()
                                               .bridgeOptions.rgbImgDimensions))
                {
                    log()->error("{} Unable open the video writer for the camera named {}.",
                                 logPrefix,
                                 camera);
                    return false;
                }
            } else
            {
                if (!this->createFramesFolder(m_videoWriters[camera].rgb, camera, "rgb"))
                {
                    log()->error("{} Unable to create the folder to store the frames for the "
                                 "camera named {}.",
                                 logPrefix,
                                 camera);
                    return false;
                }
            }
            ok = ok
                 && m_bufferManager.addChannel({"camera::" + camera + "::rgb",
                                                {1, 1}, //
                                                {"timestamp"}});
        }

        ok = ok && m_cameraBridge->getRGBDCamerasList(m_rgbdCamerasList);
        for (const auto& camera : m_rgbdCamerasList)
        {
            if (m_videoWriters[camera].rgb->saveMode == VideoWriter::SaveMode::Video)
            {
                if (!this->openVideoWriter(m_videoWriters[camera].rgb,
                                           camera,
                                           "rgb",
                                           m_cameraBridge->getMetaData()
                                               .bridgeOptions.rgbdImgDimensions))
                {
                    log()->error("{} Unable open the video writer for the rgbd camera named {}.",
                                 logPrefix,
                                 camera);
                    return false;
                }
            } else
            {
                if (!this->createFramesFolder(m_videoWriters[camera].rgb, camera, "rgb"))
                {
                    log()->error("{} Unable to create the folder to store the frames for the "
                                 "camera named {}.",
                                 logPrefix,
                                 camera);
                    return false;
                }
            }
            if (m_videoWriters[camera].depth->saveMode == VideoWriter::SaveMode::Video)
            {
                if (!this->openVideoWriter(m_videoWriters[camera].depth,
                                           camera,
                                           "depth",
                                           m_cameraBridge->getMetaData()
                                               .bridgeOptions.rgbdImgDimensions))
                {
                    log()->error("{} Unable open the video writer for the rgbd camera named {}.",
                                 logPrefix,
                                 camera);
                    return false;
                }
            } else
            {
                if (!this->createFramesFolder(m_videoWriters[camera].depth, camera, "depth"))
                {
                    log()->error("{} Unable to create the folder to store the frames for the "
                                 "camera named {}.",
                                 logPrefix,
                                 camera);
                    return false;
                }
            }

            ok = ok
                 && m_bufferManager.addChannel({"camera::" + camera + "::rgb",
                                                {1, 1}, //
                                                {"timestamp"}});

            ok = ok
                 && m_bufferManager.addChannel({"camera::" + camera + "::depth",
                                                {1, 1}, //
                                                {"timestamp"}});
        }

        if (ok)
        {
            // using C++17 it is not possible to use a structured binding in the for loop, i.e. for
            // (auto& [key, val] : m_videoWriters) since Lambda implicit capture fails with variable
            // declared from structured binding.
            // As explained in http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2017/p0588r1.html
            // If a lambda-expression [...] captures a structured binding (explicitly or
            // implicitly), the program is ill-formed.
            // you can find further information here:
            // https://stackoverflow.com/questions/46114214/lambda-implicit-capture-fails-with-variable-declared-from-structured-binding
            // Note if one day we will support c++20 we can use structured binding see
            // https://en.cppreference.com/w/cpp/language/structured_binding
            for (auto iter = m_videoWriters.begin(); iter != m_videoWriters.end(); ++iter)
            {
                // start a separate the thread for each camera
                iter->second.videoThread
                    = std::thread([this, iter] { this->recordVideo(iter->first, iter->second); });
            }
        }
    }

    ok = ok
         && m_bufferManager.setSaveCallback(
             [this](const std::string& filePrefix,
                    const robometry::SaveCallbackSaveMethod& method) -> bool {
                 return this->saveCallback(filePrefix, method);
             });

    if (ok)
    {
        return start();
    }

    return ok;
}

bool YarpRobotLoggerDevice::openVideoWriter(
    std::shared_ptr<VideoWriter::ImageSaver> imageSaver,
    const std::string& camera,
    const std::string& imageType,
    const std::unordered_map<std::string, std::pair<std::size_t, std::size_t>>& imgDimensions)
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::openVideoWriter]";
    if (imageSaver == nullptr)
    {
        log()->error("{} It seems that the camera named {} do not support {}. This shouldn't be "
                     "possible.",
                     logPrefix,
                     camera,
                     imageType);
        return false;
    }

    const auto imgDimension = imgDimensions.find(camera);
    const auto videoWriter = m_videoWriters.find(camera);

    if (imgDimension == imgDimensions.cend() || videoWriter == m_videoWriters.cend())
    {
        log()->error("{} Unable to find the dimension of the image or the video writers for the "
                     "camera named {}.",
                     logPrefix,
                     camera);
        return false;
    }

    std::lock_guard guard(imageSaver->mutex);
    imageSaver->writer
        = std::make_shared<cv::VideoWriter>("output_" + camera + "_" + imageType + ".mp4",
                                            cv::VideoWriter::fourcc(m_videoCodecCode.at(0),
                                                                    m_videoCodecCode.at(1),
                                                                    m_videoCodecCode.at(2),
                                                                    m_videoCodecCode.at(3)),
                                            videoWriter->second.fps,
                                            cv::Size(imgDimension->second.first,
                                                     imgDimension->second.second),
                                            "rgb" == imageType);
    return true;
}

bool YarpRobotLoggerDevice::createFramesFolder(std::shared_ptr<VideoWriter::ImageSaver> imageSaver,
                                               const std::string& camera,
                                               const std::string& imageType)
{
    namespace fs = std::filesystem;

    constexpr auto logPrefix = "[YarpRobotLoggerDevice::createFramesFolder]";
    if (imageSaver == nullptr)
    {
        log()->error("{} It seems that the camera named {} do not support {}. This shouldn't be "
                     "possible.",
                     logPrefix,
                     camera,
                     imageType);
        return false;
    }

    imageSaver->framesPath = "output_" + camera + "_" + imageType;
    std::lock_guard guard(imageSaver->mutex);
    std::filesystem::create_directory(imageSaver->framesPath);
    return true;
}

void YarpRobotLoggerDevice::unpackIMU(Eigen::Ref<const analog_sensor_t> signal,
                                      Eigen::Ref<accelerometer_t> accelerometer,
                                      Eigen::Ref<gyro_t> gyro,
                                      Eigen::Ref<orientation_t> orientation)
{
    // the output consists 12 double, organized as follows:
    //  euler angles [3]
    // linear acceleration [3]
    // angular speed [3]
    // magnetic field [3]
    // http://wiki.icub.org/wiki/Inertial_Sensor
    orientation = signal.segment<3>(0);
    accelerometer = signal.segment<3>(3);
    gyro = signal.segment<3>(6);
}

void YarpRobotLoggerDevice::lookForExogenousSignals()
{
    yarp::profiler::NetworkProfiler::ports_name_set yarpPorts;

    using namespace std::chrono_literals;

    auto time = BipedalLocomotion::clock().now();
    auto oldTime = time;
    auto wakeUpTime = time;
    const std::chrono::nanoseconds lookForExogenousSignalPeriod = 1s;
    m_lookForNewExogenousSignalIsRunning = true;

    auto connectToExogeneous = [this](auto& signals) -> void {
        for (auto& [name, signal] : signals)
        {
            std::lock_guard<std::mutex> lock(signal.mutex);
            if (signal.connected)
            {
                continue;
            }

            // try to connect to the signal
            signal.connected = signal.connect();

            // if the connection is successful, get the metadata
            // this is required only for the vectors collection signal
            if constexpr (std::is_same_v<std::decay_t<decltype(signal)>,
                                         typename decltype(this->m_vectorsCollectionSignals)::mapped_type>)
            {
                if (!signal.connected)
                {
                    continue;
                }

                log()->info("[YarpRobotLoggerDevice::lookForExogenousSignals] Attempt to get the "
                            "metadata for the vectors collection signal named: {}",
                            name);

                if (!signal.client.getMetadata(signal.metadata))
                {
                    log()->warn("[YarpRobotLoggerDevice::lookForExogenousSignals] Unable to get "
                                "the metadata for the signal named: {}. The exogenous signal will "
                                "not contain the metadata.",
                                name);
                }
            }
        }
    };

    while (m_lookForNewExogenousSignalIsRunning)
    {
        // detect if a clock has been reset
        oldTime = time;
        time = BipedalLocomotion::clock().now();

        // if the current time is lower than old time, the timer has been reset.
        if ((time - oldTime).count() < 1e-12)
        {
            wakeUpTime = time;
        }
        wakeUpTime += lookForExogenousSignalPeriod;

        // try to connect to the exogenous signals
        connectToExogeneous(m_vectorsCollectionSignals);
        connectToExogeneous(m_vectorSignals);

        // release the CPU
        BipedalLocomotion::clock().yield();

        // sleep
        BipedalLocomotion::clock().sleepUntil(wakeUpTime);
    }
}

bool YarpRobotLoggerDevice::hasSubstring(const std::string& str,
                                         const std::vector<std::string>& substrings) const
{
    for (const auto& substring : substrings)
    {
        if (str.find(substring) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

void YarpRobotLoggerDevice::lookForNewLogs()
{
    using namespace std::chrono_literals;
    yarp::profiler::NetworkProfiler::ports_name_set yarpPorts;
    constexpr auto textLoggingPortPrefix = "/log/";

    auto time = BipedalLocomotion::clock().now();
    auto oldTime = time;
    auto wakeUpTime = time;
    const auto lookForNewLogsPeriod = 2s;
    m_lookForNewLogsIsRunning = true;

    while (m_lookForNewLogsIsRunning)
    {
        // detect if a clock has been reset
        oldTime = time;
        time = BipedalLocomotion::clock().now();
        // if the current time is lower than old time, the timer has been reset.
        if ((time - oldTime).count() < 1e-12)
        {
            wakeUpTime = time;
        }
        wakeUpTime += lookForNewLogsPeriod;

        // check for new messages
        yarp::profiler::NetworkProfiler::getPortsList(yarpPorts);
        for (const auto& port : yarpPorts)
        {
            // check if the port has not be already connected if exits, its resposive
            // it is a text logging port and it should be logged
            if ((port.name.rfind(textLoggingPortPrefix, 0) == 0)
                && (m_textLoggingPortNames.find(port.name) == m_textLoggingPortNames.end())
                && (m_textLoggingSubnames.empty()
                    || this->hasSubstring(port.name, m_textLoggingSubnames))
                && yarp::os::Network::exists(port.name))
            {
                m_textLoggingPortNames.insert(port.name);
                yarp::os::Network::connect(port.name, m_textLoggingPortName, "udp");
            }
        }

        // release the CPU
        BipedalLocomotion::clock().yield();

        // sleep
        BipedalLocomotion::clock().sleepUntil(wakeUpTime);
    }
}

void YarpRobotLoggerDevice::recordVideo(const std::string& cameraName, VideoWriter& writer)
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::recordVideo]";

    auto time = BipedalLocomotion::clock().now();
    auto oldTime = time;
    auto wakeUpTime = time;
    writer.recordVideoIsRunning = true;
    const auto recordVideoPeriod = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(1.0 / double(writer.fps)));

    unsigned int imageIndex = 0;

    while (writer.recordVideoIsRunning)
    {
        // detect if a clock has been reset
        oldTime = time;
        time = BipedalLocomotion::clock().now();
        // if the current time is lower than old time, the timer has been reset.
        if ((time - oldTime).count() < 1e-12)
        {
            wakeUpTime = time;
        }
        wakeUpTime += recordVideoPeriod;

        // get the frame from the camera
        if (writer.rgb != nullptr)
        {
            if (!m_cameraBridge->getColorImage(cameraName, writer.rgb->frame))
            {
                log()->info("{} Unable to get the frame of the camera named: {}. The previous "
                            "frame "
                            "will be used.",
                            logPrefix,
                            cameraName);
            }

            // save the frame in the video writer
            if (writer.rgb->saveMode == VideoWriter::SaveMode::Video)
            {
                std::lock_guard<std::mutex> lock(writer.rgb->mutex);
                writer.rgb->writer->write(writer.rgb->frame);
            } else
            {
                assert(writer.rgb->saveMode == VideoWriter::SaveMode::Frame);

                const std::filesystem::path imgPath
                    = writer.rgb->framesPath / ("img_" + std::to_string(imageIndex) + ".png");

                cv::imwrite(imgPath.string(), writer.rgb->frame);

                // lock the the buffered manager mutex
                std::lock_guard lock(m_bufferManagerMutex);

                // TODO here we may save the frame itself
                m_bufferManager.push_back(std::chrono::duration<double>(time).count(),
                                          std::chrono::duration<double>(time).count(),
                                          "camera::" + cameraName + "::rgb");
            }
        }

        if (writer.depth != nullptr)
        {
            if (!m_cameraBridge->getDepthImage(cameraName, writer.depth->frame))
            {
                log()->info("{} Unable to get the frame of the camera named: {}. The previous "
                            "frame "
                            "will be used.",
                            logPrefix,
                            cameraName);

            } else
            {
                // If a new frame arrived the we should scale it
                writer.depth->frame = writer.depth->frame * writer.depthScale;
            }

            if (writer.depth->saveMode == VideoWriter::SaveMode::Video)
            {
                // we need to convert the image to 8bit this is required by the video writer
                cv::Mat image8Bit;
                writer.depth->frame.convertTo(image8Bit, CV_8UC1);

                // save the frame in the video writer
                std::lock_guard<std::mutex> lock(writer.depth->mutex);
                writer.depth->writer->write(image8Bit);
            } else
            {
                assert(writer.depth->saveMode == VideoWriter::SaveMode::Frame);

                const std::filesystem::path imgPath
                    = writer.depth->framesPath / ("img_" + std::to_string(imageIndex) + ".png");

                // convert the image into 16bit grayscale image
                cv::Mat image16Bit;
                writer.depth->frame.convertTo(image16Bit, CV_16UC1);
                cv::imwrite(imgPath.string(), image16Bit);

                // lock the the buffered manager mutex
                std::lock_guard lock(m_bufferManagerMutex);

                // TODO here we may save the frame itself
                m_bufferManager.push_back(std::chrono::duration<double>(time).count(),
                                          std::chrono::duration<double>(time).count(),
                                          "camera::" + cameraName + "::depth");
            }
        }

        // increase the index
        imageIndex++;

        // release the CPU
        BipedalLocomotion::clock().yield();

        if (wakeUpTime < BipedalLocomotion::clock().now())
        {
            log()->info("{} The video thread spent more time than expected to save the camera "
                        "named: {}.",
                        logPrefix,
                        cameraName);
        }

        // sleep
        BipedalLocomotion::clock().sleepUntil(wakeUpTime);
    }
}


void YarpRobotLoggerDevice::run()
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::run]";
    m_vectorCollectionRTDataServer.clearData();

    // get the data
    if (!m_robotSensorBridge->advance())
    {
        log()->error("{} Could not advance sensor bridge.", logPrefix);
    }

    const double time = std::chrono::duration<double>(BipedalLocomotion::clock().now()).count();

    std::lock_guard lock(m_bufferManagerMutex);

    Eigen::Matrix<double, 1, 1> timeData;
    timeData << time;

    std::string signalName = "";
    std::string rtSignalName = "";

    rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(TIMESTAMPS_NAME);
    m_vectorCollectionRTDataServer.populateData(rtSignalName, timeData);

    if (m_streamJointStates)
    {
        if (m_robotSensorBridge->getJointPositions(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, JOINT_STATE_POSITIONS_NAME);
            rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(JOINT_STATE_POSITIONS_NAME);
            m_vectorCollectionRTDataServer.populateData(rtSignalName, m_jointSensorBuffer);
        }
        if (m_robotSensorBridge->getJointVelocities(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, JOINT_STATE_VELOCITIES_NAME);
            rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(JOINT_STATE_VELOCITIES_NAME);
            m_vectorCollectionRTDataServer.populateData(rtSignalName, m_jointSensorBuffer);
        }
        if (m_robotSensorBridge->getJointAccelerations(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, JOINT_STATE_ACCLERATIONS_NAME);
            rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(JOINT_STATE_ACCLERATIONS_NAME);
            m_vectorCollectionRTDataServer.populateData(rtSignalName, m_jointSensorBuffer);
        }
        if (m_robotSensorBridge->getJointTorques(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, JOINT_STATE_TORQUES_NAME);
            rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(JOINT_STATE_TORQUES_NAME);
            m_vectorCollectionRTDataServer.populateData(rtSignalName, m_jointSensorBuffer);
        }
    }

    if (m_streamMotorStates)
    {
        if (m_robotSensorBridge->getMotorPositions(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, MOTOR_STATE_POSITIONS_NAME);
            rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(MOTOR_STATE_POSITIONS_NAME);
            m_vectorCollectionRTDataServer.populateData(rtSignalName, m_jointSensorBuffer);
        }
        if (m_robotSensorBridge->getMotorVelocities(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, MOTOR_STATE_VELOCITIES_NAME);
            rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(MOTOR_STATE_VELOCITIES_NAME);
            m_vectorCollectionRTDataServer.populateData(rtSignalName, m_jointSensorBuffer);
        }
        if (m_robotSensorBridge->getMotorAccelerations(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, MOTOR_STATE_ACCELERATIONS_NAME);
            rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(MOTOR_STATE_ACCELERATIONS_NAME);
            m_vectorCollectionRTDataServer.populateData(rtSignalName, m_jointSensorBuffer);
        }
        if (m_robotSensorBridge->getMotorCurrents(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, MOTOR_STATE_CURRENTS_NAME);
            rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(MOTOR_STATE_CURRENTS_NAME);
            m_vectorCollectionRTDataServer.populateData(rtSignalName, m_jointSensorBuffer);
        }
    }

    if (m_streamMotorPWM)
    {
        if (m_robotSensorBridge->getMotorPWMs(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, MOTOR_STATE_PWM_NAME);
            rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(MOTOR_STATE_CURRENTS_NAME);
            m_vectorCollectionRTDataServer.populateData(rtSignalName, m_jointSensorBuffer);
        }
    }

    if (m_streamPIDs)
    {
        if (m_robotSensorBridge->getPidPositions(m_jointSensorBuffer))
        {
            m_bufferManager.push_back(m_jointSensorBuffer, time, MOTOR_STATE_PIDS_NAME);
            rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(MOTOR_STATE_PIDS_NAME);
            m_vectorCollectionRTDataServer.populateData(rtSignalName, m_jointSensorBuffer);
        }
    }

    if (m_streamFTSensors)
    {
        for (const auto& sensorName : m_robotSensorBridge->getSixAxisForceTorqueSensorsList())
        {
            if (m_robotSensorBridge->getSixAxisForceTorqueMeasurement(sensorName, m_ftBuffer))
            {
                signalName = std::string(FTS_NAME) + std::string(TREE_DELIM) + sensorName;
                m_bufferManager.push_back(m_ftBuffer, time, signalName);
                rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(FTS_NAME) + std::string(TREE_DELIM) + sensorName;
                m_vectorCollectionRTDataServer.populateData(rtSignalName, m_ftBuffer);
            }
        }
    }

    if (m_streamTemperatureSensors)
    {
        for (const auto& sensorName : m_robotSensorBridge->getTemperatureSensorsList())
        {
            if (m_robotSensorBridge->getTemperature(sensorName, m_ftTemperatureBuffer))
            {
                signalName = std::string(TEMPERATURE_NAME) + std::string(TREE_DELIM) + sensorName;
                m_bufferManager.push_back({m_ftTemperatureBuffer}, time, signalName);

                Eigen::Matrix<double, 1, 1> temperatureData;
                temperatureData << m_ftTemperatureBuffer;
                rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(TEMPERATURE_NAME) + std::string(TREE_DELIM) + sensorName;
                m_vectorCollectionRTDataServer.populateData(rtSignalName, temperatureData);
            }
        }
    }

    if(m_streamInertials)
    {
        for (const auto& sensorName : m_robotSensorBridge->getGyroscopesList())
        {
            if (m_robotSensorBridge->getGyroscopeMeasure(sensorName, m_gyroBuffer))
            {
                signalName = std::string(GYROS_NAME) + std::string(TREE_DELIM) + sensorName;
                m_bufferManager.push_back(m_gyroBuffer, time, signalName);
                rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(GYROS_NAME) + std::string(TREE_DELIM) + sensorName;
                m_vectorCollectionRTDataServer.populateData(rtSignalName, m_gyroBuffer);
            }
        }

        // pack the data for the accelerometer
        for (const auto& sensorName : m_robotSensorBridge->getLinearAccelerometersList())
        {
            if(m_robotSensorBridge->getLinearAccelerometerMeasurement(sensorName, m_acceloremeterBuffer))
            {
                signalName = std::string(ACCELEROMETERS_NAME) + std::string(TREE_DELIM) + sensorName;
                m_bufferManager.push_back(m_acceloremeterBuffer, time, signalName);
                rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(ACCELEROMETERS_NAME) + std::string(TREE_DELIM) + sensorName;
                m_vectorCollectionRTDataServer.populateData(rtSignalName, m_acceloremeterBuffer);
            }
        }

        // pack the data for the orientations
        for (const auto& sensorName : m_robotSensorBridge->getOrientationSensorsList())
        {
            if (m_robotSensorBridge->getOrientationSensorMeasurement(sensorName, m_orientationBuffer))
            {
                signalName = std::string(ORIENTATIONS_NAME) + std::string(TREE_DELIM) + sensorName;
                m_bufferManager.push_back(m_orientationBuffer, time, signalName);
                rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(ORIENTATIONS_NAME) + std::string(TREE_DELIM) + sensorName;
                m_vectorCollectionRTDataServer.populateData(rtSignalName, m_orientationBuffer);
            }
        }

        for (const auto& sensorName : m_robotSensorBridge->getMagnetometersList())
        {
            if (m_robotSensorBridge->getMagnetometerMeasurement(sensorName, m_magnemetometerBuffer))
            {
                signalName = std::string(MAGNETOMETERS_NAME) + std::string(TREE_DELIM) + sensorName;
                m_bufferManager.push_back(m_magnemetometerBuffer, time, signalName);
                rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(MAGNETOMETERS_NAME) + std::string(TREE_DELIM) + sensorName;
                m_vectorCollectionRTDataServer.populateData(rtSignalName, m_magnemetometerBuffer);
            }
        }
    }

    // an IMU contains a gyro accelerometer and an orientation sensor
    for (const auto& sensorName : m_robotSensorBridge->getIMUsList())
    {
        if (m_robotSensorBridge->getIMUMeasurement(sensorName, m_analogSensorBuffer))
        {
            // it will return a tuple containing the Accelerometer, the gyro and the orientation
            this->unpackIMU(m_analogSensorBuffer,
                            m_acceloremeterBuffer,
                            m_gyroBuffer,
                            m_orientationBuffer);

            signalName = std::string(ACCELEROMETERS_NAME) + std::string(TREE_DELIM) + sensorName;
            m_bufferManager.push_back(m_acceloremeterBuffer, time, signalName);
            rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(ACCELEROMETERS_NAME) + std::string(TREE_DELIM) + sensorName;
            m_vectorCollectionRTDataServer.populateData(rtSignalName, m_acceloremeterBuffer);

            signalName = std::string(GYROS_NAME) + std::string(TREE_DELIM) + sensorName;
            m_bufferManager.push_back(m_gyroBuffer, time, signalName);
            rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(GYROS_NAME) + std::string(TREE_DELIM) + sensorName;
            m_vectorCollectionRTDataServer.populateData(rtSignalName, m_gyroBuffer);

            signalName = std::string(ORIENTATIONS_NAME) + std::string(TREE_DELIM) + sensorName;
            m_bufferManager.push_back(m_orientationBuffer, time, signalName);
            rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(ORIENTATIONS_NAME) + std::string(TREE_DELIM) + sensorName;
            m_vectorCollectionRTDataServer.populateData(rtSignalName, m_orientationBuffer);
        }
    }

    if (m_streamCartesianWrenches)
    {
        for (const auto& cartesianWrenchName : m_robotSensorBridge->getCartesianWrenchesList())
        {
            if (m_robotSensorBridge->getCartesianWrench(cartesianWrenchName, m_ftBuffer))
            {
                signalName = std::string(CARTESIAN_WRENCHES_NAME) + std::string(TREE_DELIM) + cartesianWrenchName;
                m_bufferManager.push_back(m_ftBuffer, time, signalName);
                rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + std::string(CARTESIAN_WRENCHES_NAME) + std::string(TREE_DELIM) + cartesianWrenchName;
                m_vectorCollectionRTDataServer.populateData(rtSignalName, m_ftBuffer);
            }
        }
    }

    for (auto& [name, signal] : m_vectorsCollectionSignals)
    {
        std::lock_guard<std::mutex> lock(signal.mutex);

        BipedalLocomotion::YarpUtilities::VectorsCollection* externalSignalCollection = signal.client.readData(false);
        if (externalSignalCollection != nullptr)
        {
            for (const auto& [key, vector] : externalSignalCollection->vectors)
            {
                signalName = signal.signalName + std::string(TREE_DELIM) + key;
                m_bufferManager.push_back(vector, time, signalName);
                rtSignalName = std::string(ROBOT_RT_ROOT_NAME) + std::string(TREE_DELIM) + signalName;
                m_vectorCollectionRTDataServer.populateData(rtSignalName, vector);
            }
        }
    }

    int bufferportSize = m_textLoggingPort.getPendingReads();
    BipedalLocomotion::TextLoggingEntry msg;

    while (bufferportSize > 0)
    {
        yarp::os::Bottle* b = m_textLoggingPort.read(false);
        if (b != nullptr)
        {
            msg = BipedalLocomotion::TextLoggingEntry::deserializeMessage(*b, std::to_string(time));
            if (msg.isValid)
            {
                signalName = msg.portSystem + "::" + msg.portPrefix + "::" + msg.processName
                                 + "::p" + msg.processPID;

                // matlab does not support the character - as a key of a struct
                findAndReplaceAll(signalName, "-", "_");

                // if it is the first time this signal is seen by the device the channel is
                // added
                if (m_textLogsStoredInManager.find(signalName)
                    == m_textLogsStoredInManager.end())
                {
                    m_bufferManager.addChannel({signalName, {1, 1}});
                    m_textLogsStoredInManager.insert(signalName);
                }

            }
            bufferportSize = m_textLoggingPort.getPendingReads();
        } else
        {
            break;
        }
    }

   m_vectorCollectionRTDataServer.sendData();
}

bool YarpRobotLoggerDevice::saveCallback(const std::string& fileName,
                                         const robometry::SaveCallbackSaveMethod& method)
{
    constexpr auto logPrefix = "[YarpRobotLoggerDevice::saveCallback]";

    auto codeStatus = [](const std::string& cmd, const std::string& head) -> std::string {
        std::stringstream processStream, stream;

        // run the process
        TinyProcessLib::Process process(cmd, "", [&](const char* bytes, size_t n) -> void {
            processStream << std::string(bytes, n);
        });

        // if the process status is ok we can save the output
        auto exitStatus = process.get_exit_status();
        if (exitStatus == 0)
        {
            stream << "### " << head << std::endl;
            stream << "```" << std::endl;
            stream << processStream.str() << std::endl;
            stream << "```" << std::endl;
        }
        return stream.str();
    };

    auto saveVideo = [&fileName, logPrefix](std::shared_ptr<VideoWriter::ImageSaver> imageSaver,
                                            const std::string& camera,
                                            const std::string& videoTypePostfix) -> bool {
        if (imageSaver == nullptr)
        {
            log()->error("{} The camera named {} do not expose the rgb image. This should't be "
                         "possible.",
                         logPrefix,
                         camera);
            return false;
        }

        std::string temp = fileName + "_" + camera + "_" + videoTypePostfix;
        std::string oldName = "output_" + camera + "_" + videoTypePostfix;

        // release the writer
        std::lock_guard<std::mutex> lock(imageSaver->mutex);
        if (imageSaver->saveMode == VideoWriter::SaveMode::Video)
        {
            // the name of the files contains mp4
            temp += ".mp4";
            oldName += ".mp4";

            imageSaver->writer->release();
        }

        // rename the file associated to the camera
        std::filesystem::rename(oldName, temp);

        return true;
    };

    // save the video if there is any
    for (const auto& camera : m_rgbCamerasList)
    {
        if (!saveVideo(m_videoWriters[camera].rgb, camera, "rgb"))
        {
            log()->error("{} Unable to save the rgb for the camera named {}", logPrefix, camera);
            return false;
        }

        if (method != robometry::SaveCallbackSaveMethod::periodic)
        {
            continue;
        }

        if (m_videoWriters[camera].rgb->saveMode == VideoWriter::SaveMode::Video)
        {
            if (!this->openVideoWriter(m_videoWriters[camera].rgb,
                                       camera,
                                       "rgb",
                                       m_cameraBridge->getMetaData().bridgeOptions.rgbImgDimensions))
            {
                log()->error("{} Unable to open a video writer fro the camera named {}.",
                             logPrefix,
                             camera);
                return false;
            }
        } else
        {
            if (!this->createFramesFolder(m_videoWriters[camera].rgb, camera, "rgb"))
            {
                log()->error("{} Unable to create the folder associated to the frames of the "
                             "camera named {}.",
                             logPrefix,
                             camera);
                return false;
            }
        }
    }

    for (const auto& camera : m_rgbdCamerasList)
    {
        if (!saveVideo(m_videoWriters[camera].rgb, camera, "rgb"))
        {
            log()->error("{} Unable to save the rgb for the camera named {}", logPrefix, camera);
            return false;
        }

        if (!saveVideo(m_videoWriters[camera].depth, camera, "depth"))
        {
            log()->error("{} Unable to save the depth for the camera named {}", logPrefix, camera);
            return false;
        }

        if (method != robometry::SaveCallbackSaveMethod::periodic)
        {
            continue;
        }

        if (m_videoWriters[camera].rgb->saveMode == VideoWriter::SaveMode::Video)
        {

            if (!this->openVideoWriter(m_videoWriters[camera].rgb,
                                       camera,
                                       "rgb",
                                       m_cameraBridge->getMetaData()
                                           .bridgeOptions.rgbdImgDimensions))
            {
                log()->error("{} Unable to open a video writer fro the camera named {}.",
                             logPrefix,
                             camera);
                return false;
            }
        } else
        {
            if (!this->createFramesFolder(m_videoWriters[camera].rgb, camera, "rgb"))
            {
                log()->error("{} Unable to create the folder associated to the frames of the "
                             "camera named {}.",
                             logPrefix,
                             camera);
                return false;
            }
        }
        if (m_videoWriters[camera].depth->saveMode == VideoWriter::SaveMode::Video)
        {
            if (!this->openVideoWriter(m_videoWriters[camera].depth,
                                       camera,
                                       "depth",
                                       m_cameraBridge->getMetaData()
                                           .bridgeOptions.rgbdImgDimensions))
            {
                log()->error("{} Unable to open a video writer for the depth camera named {}.",
                             logPrefix,
                             camera);
                return false;
            }
        } else
        {
            if (!this->createFramesFolder(m_videoWriters[camera].depth, camera, "depth"))
            {
                log()->error("{} Unable to create the folder associated to the depth frames of "
                             "the "
                             "camera named {}.",
                             logPrefix,
                             camera);
                return false;
            }
        }
    }

    // save the status of the code
    std::ofstream file(fileName + ".md");
    file << "# " << fileName << std::endl;
    file << "File containing all the installed software required to replicate the experiment.  "
         << std::endl;

    if (m_codeStatusCmdPrefixes.empty())
    {
        file << codeStatus("bash "
                           "${ROBOTOLOGY_SUPERBUILD_SOURCE_DIR}/scripts/robotologyGitStatus.sh",
                           "ROBOTOLOGY");
        file << codeStatus("apt list --installed", "APT");
    } else
    {
        for (const auto& prefix : m_codeStatusCmdPrefixes)
        {
            file << "## `" << prefix << "`" << std::endl;
            file << codeStatus(prefix
                                   + " \"bash "
                                     "${ROBOTOLOGY_SUPERBUILD_SOURCE_DIR}/scripts/"
                                     "robotologyGitStatus.sh\"",
                               "ROBOTOLOGY");
            file << codeStatus(prefix + " \"apt list --installed\"", "APT");
        }
    }

    file.close();

    return true;
}

bool YarpRobotLoggerDevice::detachAll()
{
    if (isRunning())
    {
        stop();
    }

    return true;
}

bool YarpRobotLoggerDevice::close()
{
    // stop all the video thread
    for (auto& [cameraName, writer] : m_videoWriters)
    {
        writer.recordVideoIsRunning = false;
    }

    // close all the thread associated to the video logging
    for (auto& [cameraName, writer] : m_videoWriters)
    {
        if (writer.videoThread.joinable())
        {
            writer.videoThread.join();
            writer.videoThread = std::thread();
        }
    }

    // close the thread associated to the text logging polling
    m_lookForNewLogsIsRunning = false;
    if (m_lookForNewLogsThread.joinable())
    {
        m_lookForNewLogsThread.join();
        m_lookForNewLogsThread = std::thread();
    }

    m_lookForNewExogenousSignalIsRunning = false;
    if (m_lookForNewExogenousSignalThread.joinable())
    {
        m_lookForNewExogenousSignalThread.join();
        m_lookForNewExogenousSignalThread = std::thread();
    }

    return true;
}
