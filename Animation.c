
/******************************************************************************************
*  Purpose:                                                                               *
*    Play Blend and Mix Animations Trigger and Transition Between animations              *
*    Can Play Seperate animations on Upper Body and Lower Body: Sword Slash while Walking *
*    Can Rotate The Head and Spine of humanoid character independently,                   *
*    Allows to look around and turn the body                                              *
*  Good To Know:                                                                          *
*    Stores bone matrices into Matrix3x4Half format and sends to GPU within Textures      *
*    Scale interpolation is disabled for now                                              *
*  Author:                                                                                *
*    Anilcan Gulkaya 2024 anilcangulkaya7@gmail.com github @benanil                       *
*******************************************************************************************/

#include "Animation.h"
// #include "Scene.h"
// #include "SceneRenderer.h"
#include "Platform.h"
#include "Algorithm.h"
#include "Math/Half.h"

// <<<<<<<        prefab         >>>>>>>>>>>>


int Prefab_FindAnimRootNodeIndex(SceneBundle* prefab)
{
    if (prefab->skins == NULL)
        return 0;

    ASkin skin = prefab->skins[0];
    if (skin.skeleton != -1) 
        return skin.skeleton;
    
    // search for Armature name, and also record the node that has most children
    int armatureIdx = -1;
    int maxChilds   = 0;
    int maxChildIdx = 0;
    // maybe recurse to find max children
    for (int i = 0; i < prefab->numNodes; i++)
    {
        if (StrCMP16(prefab->nodes[i].name, "Armature")) {
            armatureIdx = i;
            break;
        }
    
        int numChildren = prefab->nodes[i].numChildren;
        if (numChildren > maxChilds) {
            maxChilds = numChildren;
            maxChildIdx = i;
        }
    }
    
    int skeletonNode = armatureIdx != -1 ? armatureIdx : maxChildIdx;
    return skeletonNode;
}

int Prefab_FindNodeFromName(SceneBundle* prefab, const char* name)
{
    int len = StringLength(name);
    for (int i = 0; i < prefab->numNodes; i++)
    {
        if (StringEqual(prefab->nodes[i].name, name, len))
            return i;
    }
    AX_WARN("couldn't find node from name %s", name);
    return 0;
}


void StartAnimationSystem()
{ }

static void SetBoneNode(Prefab* prefab, ANode** node, int* index, const char* name)
{
    *index = Prefab_FindNodeFromName(&prefab->sceneBundle, name);
    *node = &prefab->nodes[*index];
}

void CreateAnimationController(Prefab* prefab, AnimationController* result, bool humanoid, int lowerBodyStart)
{
    ASkin* skin = &prefab->skins[0];
    if (skin == NULL) {
        AX_WARN("skin is null %s", prefab->path); return;
    }
    if (skin->numJoints > MaxBonePoses) {
        AX_WARN("number of joints is greater than max capacity %s", prefab->path); 
        return; 
    }
    result->mMatrixTex = rCreateTexture(skin->numJoints*3, 1, result->mOutMatrices, SG_PIXELFORMAT_RGBA16F, TexFlags_RawData, "AnimationMatrixTex");
    result->mRootNodeIndex = Prefab_FindAnimRootNodeIndex(&prefab->sceneBundle);
    result->mPrefab = prefab;
    result->mState = AnimState_Update;
    result->mNumNodes = prefab->numNodes;
    result->mTrigerredNorm = 0.0f;
    result->lowerBodyIdxStart = lowerBodyStart;

    ASSERT(result->mRootNodeIndex < MaxBonePoses);
    ASSERT(GetNodePtr(prefab, result->mRootNodeIndex)->numChildren > 0); // root node has to have children nodes
    
    if (!humanoid)
        return;
    
    SetBoneNode(prefab, &result->mSpineNode, &result->mSpineNodeIdx, "mixamorig:Spine");
    SetBoneNode(prefab, &result->mNeckNode , &result->mNeckNodeIdx , "mixamorig:Neck");
}

static inline void RotateNode(ANode* node, float xAngle, float yAngle)
{
    Quaternion q = QMul(QMul(QFromXAngle(xAngle), QFromYAngle(yAngle)), VecLoad(node->rotation));
    VecStore(node->rotation, q);
}

purefn Matrix4 GetNodeMatrix(ANode* node)
{
    return PositionRotationScalePtr(node->translation, node->rotation, node->scale);
}

void AnimationController_RecurseBoneMatrices(AnimationController* ac, ANode* node, Matrix4 parentMatrix)
{
    for (int c = 0; c < node->numChildren; c++)
    {
        int childIndex = node->children[c];
        ANode* children = &ac->mPrefab->nodes[childIndex];

        if (children == ac->mSpineNode && Absf(ac->mSpineYAngle) + Absf(ac->mSpineXAngle) > MATH_Epsilon) { RotateNode(children, ac->mSpineXAngle, ac->mSpineYAngle); }
        if (children == ac->mNeckNode && Absf(ac->mNeckYAngle) + Absf(ac->mSpineXAngle) > MATH_Epsilon) { RotateNode(children, ac->mNeckXAngle, ac->mNeckYAngle); }

        ac->mBoneMatrices[childIndex] = Matrix4Multiply(parentMatrix, GetNodeMatrix(children));

        AnimationController_RecurseBoneMatrices(ac, children, ac->mBoneMatrices[childIndex]);
    }
}

static void MergeAnims(Pose* pose0, Pose* pose1, float animBlend, int numNodes)
{
    for (int i = 0; i < numNodes; i++)
    {
        pose0[i].rotation    = QNLerp(pose0[i].rotation, pose1[i].rotation, animBlend); // slerp+norm?
        pose0[i].translation = VecLerp(pose0[i].translation, pose1[i].translation, animBlend);
        // pose0[i].scale    = VecLerp(pose0[i].scale, pose1[i].scale, animBlend);
    }
}

static void InitNodes(ANode* nodes, Pose* pose, int begin, int numNodes)
{
    numNodes += begin;
    for (int i = begin; i < numNodes; i++)
    {
        VecStore(nodes[i].translation, pose[i].translation);
        VecStore(nodes[i].rotation, pose[i].rotation);
        // VecStore(nodes[i].scale, pose[i].scale);
    }
}

static void InitPose(Pose* pose, ANode* nodes, int numNodes)
{
    for (int i = 0; i < numNodes; i++)
    {
        pose[i].translation = VecLoad(nodes[i].translation);
        pose[i].rotation    = VecLoad(nodes[i].rotation);
        // pose[i].scale    = VecLoad(nodes[i].scale);
    }
}

void AnimationController_SampleAnimationPose(AnimationController* ac, Pose* pose, int animIdx, float normTime)
{
    AAnimation* animation = &ac->mPrefab->animations[animIdx];
    bool reverse = normTime < 0.0f;
    normTime = Absf(normTime);
    if (reverse) normTime = MMAX(1.0f - normTime, 0.0f);

    InitPose(pose, ac->mPrefab->nodes, ac->mPrefab->numNodes);
    float realTime = normTime * animation->duration;
    
    for (int c = 0; c < animation->numChannels; c++)
    {
        AAnimChannel* channel = &animation->channels[c];
        int targetNode = channel->targetNode; // prefab->GetNodePtr();
        AAnimSampler* sampler = &animation->samplers[channel->sampler];
    
        // morph targets are not supported
        if (channel->targetPath == AAnimTargetPath_Weight)
            continue;
    
        // maybe binary search
        int beginIdx = 0, endIdx;
        while (realTime >= sampler->input[beginIdx + 1])
            beginIdx++;
        
        beginIdx = Minf(beginIdx, sampler->count - 1);
        endIdx   = beginIdx + 1;

        if (reverse) XSWAP(int, beginIdx, endIdx);

        Vector4x32f begin = ((Vector4x32f*)sampler->output)[beginIdx];
        Vector4x32f end   = ((Vector4x32f*)sampler->output)[endIdx];
    
        float beginTime = Minf(0.0001f, realTime - sampler->input[beginIdx]);
        float endTime   = Maxf(0.0001f, sampler->input[endIdx] - sampler->input[beginIdx]);
        
        if (reverse) XSWAP(float, beginTime, endTime);
        
        float t = MCLAMP01(beginTime / endTime);

        switch (channel->targetPath)
        {
            case AAnimTargetPath_Translation:
                pose[targetNode].translation = VecLerp(begin, end, t);
                break;
            case AAnimTargetPath_Rotation:
                Quaternion rot = QSlerp(begin, end, t);
                pose[targetNode].rotation = QNorm(rot); // QNormEst maybe
                break;
        //  case AAnimTargetPath_Scale:
        //      pose[targetNode].scale = VecLerp(begin, end, t);
        //      break;
        };
    }
}

// send matrices to GPU
void AnimationController_UploadBoneMatrices(AnimationController* ac)
{
    ASkin* skin = &ac->mPrefab->skins[0];
    Matrix4* invMatrices = (Matrix4*)skin->inverseBindMatrices;

    // give this, thousands of joints it will process it rapidly!
    for (int i = 0; i < skin->numJoints; i++)
    {
        Matrix4 mat = Matrix4Multiply(ac->mBoneMatrices[skin->joints[i]], invMatrices[i]);
        mat = Matrix4Transpose(mat);
        // with AVX F16C this is single instruction! vcvtps2ph 
        ConvertFloat8ToHalf8(ac->mOutMatrices[i].x, &mat.m[0][0]);
        ConvertFloat4ToHalf4(ac->mOutMatrices[i].z, &mat.m[2][0]); // this is single instruction with it as well
    }
    
    // upload anim matrix texture to the GPU
    rUpdateTexture(ac->mMatrixTex, ac->mOutMatrices);
}

void AnimationController_UploadPose(AnimationController* ac, Pose* pose)
{
    InitNodes(ac->mPrefab->nodes, pose, 0, ac->mPrefab->numNodes);

    ANode* rootNode = GetNodePtr(ac->mPrefab, ac->mRootNodeIndex);
    ac->mBoneMatrices[ac->mRootNodeIndex] = GetNodeMatrix(rootNode);

    AnimationController_RecurseBoneMatrices(ac, rootNode, ac->mBoneMatrices[ac->mRootNodeIndex]);
    AnimationController_UploadBoneMatrices(ac);
}

// when we want to play different animations with lower body and upper body
void AnimationController_UploadPoseUpperLower(AnimationController* ac, Pose* lowerPose, Pose* uperPose)
{
    // apply posess to lower body and upper body seperately, so both of it has diferrent animations
    InitNodes(ac->mPrefab->nodes, lowerPose, ac->lowerBodyIdxStart, ac->mPrefab->numNodes - ac->lowerBodyIdxStart);
    InitNodes(ac->mPrefab->nodes, uperPose, 0, ac->lowerBodyIdxStart);

    ANode* rootNode = GetNodePtr(ac->mPrefab, ac->mRootNodeIndex);
    Matrix4 rootMatrix = GetNodeMatrix(rootNode);
    ac->mBoneMatrices[ac->mRootNodeIndex] = rootMatrix;

    AnimationController_RecurseBoneMatrices(ac, rootNode, rootMatrix);
    AnimationController_UploadBoneMatrices(ac);
}

void AnimationController_PlayAnim(AnimationController* ac, int index, float norm)
{
    AnimationController_SampleAnimationPose(ac, ac->mAnimPoseA, index, norm);
    AnimationController_UploadPose(ac, ac->mAnimPoseA);
}

bool AnimationController_TriggerAnim(AnimationController* ac, int index, float transitionInTime, float transitionOutTime, eAnimTriggerOpt triggerOpt)
{
    if (AnimationController_IsTrigerred(ac)) return false; // already trigerred

    ac->mTriggerredAnim = index;
    ac->mTriggerOpt = triggerOpt;
    ac->mTransitionTime = transitionInTime;
    ac->mCurTransitionTime = transitionInTime;
    ac->mTransitionOutTime = transitionOutTime;
    if (transitionInTime < 0.02f) { // no transition requested
        ac->mState = AnimState_TriggerPlaying;
        return true;
    }

    ac->mState = AnimState_TriggerIn;
    SmallMemCpy(ac->mAnimPoseC, ac->mAnimPoseA, sizeof(ac->mAnimPoseA));
    if ((triggerOpt & eAnimTriggerOpt_ReverseOut))
        ac->mAnimTime.y = 0.0f;
    return true;
}

bool AnimationController_TriggerTransition(AnimationController* ac, float deltaTime, int targetAnim)
{
    float newNorm   = MCLAMP01((ac->mTransitionTime - ac->mCurTransitionTime) / ac->mTransitionTime);
    float animDelta = MCLAMP01(deltaTime * (1.0f / MMAX(1.0f - newNorm, MATH_Epsilon)));
    AnimationController_SampleAnimationPose(ac, ac->mAnimPoseD, targetAnim, ac->mAnimTime.y);
    MergeAnims(ac->mAnimPoseC, ac->mAnimPoseD, animDelta, ac->mNumNodes);
    ac->mCurTransitionTime -= deltaTime;
    return ac->mCurTransitionTime <= 0.0f;
}

// x, y has to be between -1.0 and 1.0
void AnimationController_EvaluateLocomotion(AnimationController* ac, float x, float y, float animSpeed)
{
    const float deltaTime = (float)GetDeltaTime();
    bool wasTriggerState = AnimationController_IsTrigerred(ac);

    if (ac->mState == AnimState_TriggerIn)
    {
        if (AnimationController_TriggerTransition(ac, deltaTime, ac->mTriggerredAnim)) 
            ac->mState = AnimState_TriggerPlaying;
    }
    else if (ac->mState == AnimState_TriggerOut)
    {
        if (!!!(ac->mTriggerOpt & eAnimTriggerOpt_ReverseOut)) 
        {
            if (AnimationController_TriggerTransition(ac, deltaTime, ac->mLastAnim))
                ac->mState = AnimState_Update;
        }
        else 
        {
            AnimationController_SampleAnimationPose(ac, ac->mAnimPoseC, ac->mTriggerredAnim, -ac->mTrigerredNorm);
            float animStep = 1.0f / ac->mPrefab->animations[ac->mTriggerredAnim].duration;
            ac->mTrigerredNorm = Clamp01f(ac->mTrigerredNorm + (animSpeed * animStep * deltaTime));
            if (ac->mTrigerredNorm >= 1.0f)
                ac->mState = AnimState_Update;
        }
    }
    else if (ac->mState == AnimState_TriggerPlaying)
    {
        AnimationController_SampleAnimationPose(ac, ac->mAnimPoseC, ac->mTriggerredAnim, ac->mTrigerredNorm);

        float animStep = 1.0f / ac->mPrefab->animations[ac->mTriggerredAnim].duration;
        ac->mTrigerredNorm = Clamp01f(ac->mTrigerredNorm + (animSpeed * animStep * deltaTime));
          
        if (ac->mTrigerredNorm >= 1.0f)
        {
            ac->mTrigerredNorm = 0.0f; // trigger stage complated
            ac->mTransitionTime = ac->mTransitionOutTime;
            ac->mCurTransitionTime = ac->mTransitionOutTime;
            ac->mState = ac->mTransitionTime < 0.02f ? AnimState_Update : AnimState_TriggerOut;
        }
    }

    int yIndex = AnimationController_GetAnim(ac, aMiddle, 0);
    // if trigerred animation is not standing, we don't have to sample walking or running animations
    if (!wasTriggerState || (wasTriggerState && !!(ac->mTriggerOpt & eAnimTriggerOpt_Standing) && Absf(y) > 0.001f))
    {
        // play and blend walking and running anims
        y = Absf(y); 
        int yi = (int)(y);
        // sample y anim
        ASSERTR(yi <= 3, return); // must be between 1 and 4
        yIndex = AnimationController_GetAnim(ac, aMiddle, yi);

        AnimationController_SampleAnimationPose(ac, ac->mAnimPoseA, yIndex, ac->mAnimTime.y);
        float yBlend = Fractf(y);

        bool shouldAnimBlendY = yi != 3 && yBlend > 0.00002f;
        if (shouldAnimBlendY)
        {
            yIndex = AnimationController_GetAnim(ac, aMiddle, yi + 1);
            AnimationController_SampleAnimationPose(ac, ac->mAnimPoseB, yIndex, ac->mAnimTime.y);
            MergeAnims(ac->mAnimPoseA, ac->mAnimPoseB, EaseOut(yBlend), ac->mNumNodes);
        }

        // if anim is two seconds animStep is 0.5 because we are using normalized value
        float yAnimStep = 1.0f / ac->mPrefab->animations[yIndex].duration;
        ac->mAnimTime.y += animSpeed * yAnimStep * deltaTime;
        ac->mAnimTime.y  = Fractf(ac->mAnimTime.y);
    }
    ac->mLastAnim = yIndex;

    if (!wasTriggerState) {
        AnimationController_UploadPose(ac, ac->mAnimPoseA);
    }
    else {
        if (!!(ac->mTriggerOpt & eAnimTriggerOpt_Standing) && y > 0.001f)
            AnimationController_UploadPoseUpperLower(ac, ac->mAnimPoseA, ac->mAnimPoseC);
        else
            AnimationController_UploadPose(ac, ac->mAnimPoseC);
    }
}

void ClearAnimationController(AnimationController* animSystem)
{
    rDeleteTexture(animSystem->mMatrixTex);
}

void DestroyAnimationSystem()
{ }
    
