// Bone dictionary (GkBoneOverride.hpp / GkGuiTabBone.hpp, 51 editable
// humanoid bones in six groups; hid 21..23 are not exposed by the reference).

export interface BoneDef {
  hid: number
  name: string
  labelZh: string
}

export interface BoneGroup {
  title: string
  bones: BoneDef[]
}

export const BONE_GROUPS: BoneGroup[] = [
  {
    title: '躯干 (Torso)',
    bones: [
      { hid: 0, name: 'Hips', labelZh: '骨盆 (Hips)' },
      { hid: 7, name: 'Spine', labelZh: '脊柱 (Spine)' },
      { hid: 8, name: 'Chest', labelZh: '胸腔 (Chest)' },
    ],
  },
  {
    title: '头颈 (Head & Neck)',
    bones: [
      { hid: 9, name: 'Neck', labelZh: '颈部 (Neck)' },
      { hid: 10, name: 'Head', labelZh: '头部 (Head)' },
    ],
  },
  {
    title: '上肢双臂 (Arms)',
    bones: [
      { hid: 11, name: 'LeftShoulder', labelZh: '左肩 (L Shoulder)' },
      { hid: 12, name: 'RightShoulder', labelZh: '右肩 (R Shoulder)' },
      { hid: 13, name: 'LeftUpperArm', labelZh: '左上臂 (L UpperArm)' },
      { hid: 14, name: 'RightUpperArm', labelZh: '右上臂 (R UpperArm)' },
      { hid: 15, name: 'LeftLowerArm', labelZh: '左小臂/手肘 (L Forearm)' },
      { hid: 16, name: 'RightLowerArm', labelZh: '右小臂/手肘 (R Forearm)' },
      { hid: 17, name: 'LeftHand', labelZh: '左手 (L Hand)' },
      { hid: 18, name: 'RightHand', labelZh: '右手 (R Hand)' },
    ],
  },
  {
    title: '下肢双腿 (Legs)',
    bones: [
      { hid: 1, name: 'LeftUpperLeg', labelZh: '左大腿 (L Thigh)' },
      { hid: 2, name: 'RightUpperLeg', labelZh: '右大腿 (R Thigh)' },
      { hid: 3, name: 'LeftLowerLeg', labelZh: '左小腿/膝盖 (L Calf)' },
      { hid: 4, name: 'RightLowerLeg', labelZh: '右小腿/膝盖 (R Calf)' },
      { hid: 5, name: 'LeftFoot', labelZh: '左足踝 (L Foot)' },
      { hid: 6, name: 'RightFoot', labelZh: '右足踝 (R Foot)' },
      { hid: 19, name: 'LeftToes', labelZh: '左脚趾 (L Toes)' },
      { hid: 20, name: 'RightToes', labelZh: '右脚趾 (R Toes)' },
    ],
  },
  {
    title: '左手手指 (L Fingers)',
    bones: [
      { hid: 24, name: 'LeftThumbProximal', labelZh: '左拇指根' },
      { hid: 25, name: 'LeftThumbIntermediate', labelZh: '左拇指中' },
      { hid: 26, name: 'LeftThumbDistal', labelZh: '左拇指尖' },
      { hid: 27, name: 'LeftIndexProximal', labelZh: '左食指根' },
      { hid: 28, name: 'LeftIndexIntermediate', labelZh: '左食指中' },
      { hid: 29, name: 'LeftIndexDistal', labelZh: '左食指尖' },
      { hid: 30, name: 'LeftMiddleProximal', labelZh: '左中指根' },
      { hid: 31, name: 'LeftMiddleIntermediate', labelZh: '左中指中' },
      { hid: 32, name: 'LeftMiddleDistal', labelZh: '左中指尖' },
      { hid: 33, name: 'LeftRingProximal', labelZh: '左无名根' },
      { hid: 34, name: 'LeftRingIntermediate', labelZh: '左无名中' },
      { hid: 35, name: 'LeftRingDistal', labelZh: '左无名尖' },
      { hid: 36, name: 'LeftLittleProximal', labelZh: '左小指根' },
      { hid: 37, name: 'LeftLittleIntermediate', labelZh: '左小指中' },
      { hid: 38, name: 'LeftLittleDistal', labelZh: '左小指尖' },
    ],
  },
  {
    title: '右手手指 (R Fingers)',
    bones: [
      { hid: 39, name: 'RightThumbProximal', labelZh: '右拇指根' },
      { hid: 40, name: 'RightThumbIntermediate', labelZh: '右拇指中' },
      { hid: 41, name: 'RightThumbDistal', labelZh: '右拇指尖' },
      { hid: 42, name: 'RightIndexProximal', labelZh: '右食指根' },
      { hid: 43, name: 'RightIndexIntermediate', labelZh: '右食指中' },
      { hid: 44, name: 'RightIndexDistal', labelZh: '右食指尖' },
      { hid: 45, name: 'RightMiddleProximal', labelZh: '右中指根' },
      { hid: 46, name: 'RightMiddleIntermediate', labelZh: '右中指中' },
      { hid: 47, name: 'RightMiddleDistal', labelZh: '右中指尖' },
      { hid: 48, name: 'RightRingProximal', labelZh: '右无名根' },
      { hid: 49, name: 'RightRingIntermediate', labelZh: '右无名中' },
      { hid: 50, name: 'RightRingDistal', labelZh: '右无名尖' },
      { hid: 51, name: 'RightLittleProximal', labelZh: '右小指根' },
      { hid: 52, name: 'RightLittleIntermediate', labelZh: '右小指中' },
      { hid: 53, name: 'RightLittleDistal', labelZh: '右小指尖' },
    ],
  },
]

export const ALL_BONES: BoneDef[] = BONE_GROUPS.flatMap((group) => group.bones)

export function boneNameByHid(hid: number): string {
  return ALL_BONES.find((bone) => bone.hid === hid)?.name ?? ''
}

export function symmetricalHid(hid: number): number {
  const pairs: Record<number, number> = {
    11: 12, 12: 11, 13: 14, 14: 13, 15: 16, 16: 15, 17: 18, 18: 17,
    1: 2, 2: 1, 3: 4, 4: 3, 5: 6, 6: 5, 19: 20, 20: 19,
  }
  if (hid in pairs) return pairs[hid]
  if (hid >= 24 && hid <= 38) return hid + 15
  if (hid >= 39 && hid <= 53) return hid - 15
  return -1
}
