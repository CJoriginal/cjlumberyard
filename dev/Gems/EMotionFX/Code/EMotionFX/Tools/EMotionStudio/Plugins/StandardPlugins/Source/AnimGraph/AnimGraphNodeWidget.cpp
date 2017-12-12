/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/

#include <EMotionFX/Tools/EMotionStudio/Plugins/StandardPlugins/Source/AnimGraph/AnimGraphNodeWidget.h>
#include <EMotionFX/Tools/EMotionStudio/EMStudioSDK/Source/EMStudioManager.h>


namespace EMStudio
{
    AnimGraphNodeWidget::AnimGraphNodeWidget(QWidget* parent)
        : QWidget(parent)
    {
    }


    AnimGraphNodeWidget::~AnimGraphNodeWidget()
    {
    }


    EMotionFX::AnimGraphInstance* AnimGraphNodeWidget::GetAnimGraphInstance() const
    {
        const EMotionFX::ActorInstance* actorInstance = CommandSystem::GetCommandManager()->GetCurrentSelection().GetSingleActorInstance();
        if (!actorInstance)
        {
            return nullptr;
        }
        
        return actorInstance->GetAnimGraphInstance();
    }
} // namespace EMStudio

#include <EMotionFX/Tools/EMotionStudio/Plugins/StandardPlugins/Source/AnimGraph/AnimGraphNodeWidget.moc>