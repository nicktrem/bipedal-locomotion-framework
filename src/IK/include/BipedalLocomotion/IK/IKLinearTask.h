/**
 * @file IKLinearTask.h
 * @authors Giulio Romualdi
 * @copyright 2021 Istituto Italiano di Tecnologia (IIT). This software may be modified and
 * distributed under the terms of the BSD-3-Clause license.
 */

#ifndef BIPEDAL_LOCOMOTION_SYSTEM_IK_LINEAR_TASK_H
#define BIPEDAL_LOCOMOTION_SYSTEM_IK_LINEAR_TASK_H

#include <BipedalLocomotion/System/LinearTask.h>
#include <BipedalLocomotion/System/ILinearTaskFactory.h>

#include <iDynTree/KinDynComputations.h>


namespace BipedalLocomotion
{
namespace IK
{

/**
 * IKLinearTask specializes a LinearTask in the case of Inverse Kinematics.
 */
struct IKLinearTask : public BipedalLocomotion::System::LinearTask
{
    virtual ~IKLinearTask() = default;

    /**
     * Set the kinDynComputations object.
     * @param kinDyn pointer to a kinDynComputations object.
     * @return True in case of success, false otherwise.
     */
    virtual bool setKinDyn(std::shared_ptr<iDynTree::KinDynComputations> kinDyn);
};

struct IKLinearTaskFactory : public ::BipedalLocomotion::System::ILinearTaskFactory<IKLinearTask>
{
    IKLinearTaskFactory();
};

} // namespace IK
} // namespace BipedalLocomotion

/**
 * BLF_REGISTER_IK_TASK is a macro that can be used to register an IKLinearTask. The key of the
 * task will be the stringified version of the Task C++ Type
 * @param _type the type of the task
 */
#define BLF_REGISTER_IK_TASK(_type) BLF_REGISTER_TASK(_type, ::BipedalLocomotion::IK::IKLinearTask, ::BipedalLocomotion::IK::IKLinearTaskFactory)

#endif // BIPEDAL_LOCOMOTION_SYSTEM_IK_LINEAR_TASK_H
