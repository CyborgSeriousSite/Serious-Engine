/* Copyright (c) 2002-2012 Croteam Ltd. 
This program is free software; you can redistribute it and/or modify
it under the terms of version 2 of the GNU General Public License as published by
the Free Software Foundation


This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#include "stdafx.h"

#include <Engine/Models/ModelObject.h>
#include <Engine/Models/ModelData.h>
#include <Engine/Models/ImportedMesh.h>
#include <Engine/Models/ImportedSkeleton.h>
#include <Engine/Models/ImportedSkeletalAnimation.h>
#include <Engine/Math/Geometry.inl>
#include <Engine/Models/Model_internal.h>
#include <Engine/Base/Stream.h>
#include <Engine/Base/ListIterator.inl>
#include <Engine/Models/Normals.h>
#include <Engine/Graphics/DrawPort.h>

#include <Engine/Templates/StaticArray.cpp>
#include <Engine/Templates/DynamicArray.cpp>
#include <Engine/Templates/Stock_CTextureData.h>

#include "EditModel.h"
#include "MipMaker.h"
#include "Script/ScriptIO.h"

#include <unordered_map>
#include <vector>
#include <memory>

namespace {

// print #define <animation name> lines for all animations into given file
void ExportAnimationNames_t(CAnimData& animData, CTStream* ostrFile, CTString strAnimationPrefix) // throw char *
{
  char chrLine[256];
  // for each animation
  for (INDEX iAnimation = 0; iAnimation < animData.ad_NumberOfAnims; iAnimation++)
  {
    // prepare one #define line (add prefix)
    sprintf(chrLine, "#define %s%s %d", strAnimationPrefix, animData.ad_Anims[iAnimation].oa_Name,
      iAnimation);
    // put it into file
    ostrFile->PutLine_t(chrLine);
  }
}

} // anonymous namespace

// Globally instanciated object containing routines for dealing with progres messages
CProgressRoutines ProgresRoutines;

// constants important to this module
#define MAX_ALLOWED_DISTANCE 0.0001f

#define  PC_ALLWAYS_ON (1UL << 30)
#define  PC_ALLWAYS_OFF (1UL << 31)

CThumbnailSettings::CThumbnailSettings( void)
{
  ts_bSet = FALSE;
}

void CThumbnailSettings::Read_t( CTStream *strFile)
{
  *strFile>>ts_bSet;
  *strFile>>ts_plLightPlacement;
  *strFile>>ts_plModelPlacement;
  *strFile>>ts_fTargetDistance;
  *strFile>>ts_vTarget;
  *strFile>>ts_angViewerOrientation;
  *strFile>>ts_LightDistance;
  *strFile>>ts_LightColor;
  *strFile>>ts_colAmbientColor;
  *strFile>>ts_PaperColor;
  *strFile>>ts_InkColor;
  *strFile>>ts_IsWinBcgTexture;
  *strFile>>ts_WinBcgTextureName;
  ts_RenderPrefs.Read_t( strFile);
}

void CThumbnailSettings::Write_t( CTStream *strFile)
{
  *strFile<<ts_bSet;
  *strFile<<ts_plLightPlacement;
  *strFile<<ts_plModelPlacement;
  *strFile<<ts_fTargetDistance;
  *strFile<<ts_vTarget;
  *strFile<<ts_angViewerOrientation;
  *strFile<<ts_LightDistance;
  *strFile<<ts_LightColor;
  *strFile<<ts_colAmbientColor;
  *strFile<<ts_PaperColor;
  *strFile<<ts_InkColor;
  *strFile<<ts_IsWinBcgTexture;
  *strFile<<ts_WinBcgTextureName;
  ts_RenderPrefs.Write_t( strFile);
}

CEditModel::CEditModel()
{
  edm_md.md_bIsEdited = TRUE; // this model data is edited
  edm_iActiveCollisionBox = 0;
}

CEditModel::~CEditModel()
{
  FORDELETELIST( CTextureDataInfo, tdi_ListNode, edm_WorkingSkins, litDel3)
  {
    ASSERT( litDel3->tdi_TextureData != NULL);
    _pTextureStock->Release( litDel3->tdi_TextureData);
    delete &litDel3.Current();
  }
}

CProgressRoutines::CProgressRoutines()
{
  SetProgressMessage = NULL;
  SetProgressRange = NULL;
  SetProgressState = NULL;
}

void CreateBoneTriangles(ImportedMesh& mesh, const FLOATmatrix3D& transform, FLOAT stretch)
{
  size_t materialIndex = -1;
  for (size_t boneIndex = 0; boneIndex < mesh.m_weightBones.size(); ++boneIndex)
  {
    const auto& weightBone = mesh.m_weightBones.at(boneIndex);

    const FLOATmatrix4D boneTransform = InverseMatrix(weightBone.m_offset);
    FLOAT4D v0(0.0f, 0.0f, 0.0f, 1.0f);
    FLOAT4D v1(0.0f, 0.25f / stretch, 0.0f, 1.0f);
    FLOAT4D v2(0.0f, 0.0f, -0.25f / stretch, 1.0f);
    v0 = v0 * boneTransform;
    v1 = v1 * boneTransform;
    v2 = v2 * boneTransform;
    const size_t baseIndex = mesh.m_vertices.size();
    mesh.m_vertices.push_back(FLOAT3D(v0(1), v0(2), v0(3)) * transform);
    mesh.m_vertices.push_back(FLOAT3D(v1(1), v1(2), v1(3)) * transform);
    mesh.m_vertices.push_back(FLOAT3D(v2(1), v2(2), v2(3)) * transform);
    for (size_t i = 0; i < 3; ++i)
    {
      mesh.m_vertices[baseIndex + i](1) *= -1.0f;
      mesh.m_vertices[baseIndex + i](3) *= -1.0f;
    }

    ImportedMesh::TWeights weight;
    weight[boneIndex] = 1.0f;
    mesh.m_verticeWeights.push_back(weight);
    mesh.m_verticeWeights.push_back(weight);
    mesh.m_verticeWeights.push_back(weight);

    if (materialIndex == -1)
    {
      materialIndex = mesh.m_materials.size();
      ImportedMesh::Material material;
      material.cm_strName = "<Autogenerated bone triangles>";
      material.cm_colColor = C_RED;
      mesh.m_materials.push_back(material);
    }

    ImportedMesh::Triangle triangle;
    triangle.ct_iVtx[0] = baseIndex + 0;
    triangle.ct_iVtx[1] = baseIndex + 1;
    triangle.ct_iVtx[2] = baseIndex + 2;
    for (size_t i = 0; i < 3; ++i)
      for (size_t j = 0; j < 3; ++j)
        triangle.ct_iTVtx[i][j] = 0;
    triangle.ct_iMaterial = materialIndex;
    mesh.m_triangles.push_back(triangle);
  }
}

//----------------------------------------------------------------------------------------------
std::vector<CEditModel::FrameGenerator> CEditModel::LoadFrameGenerators(
  const ModelScript::Animations& animations,
  const ImportedMesh& baseMesh,
  const ImportedSkeleton& skeleton,
  const FLOATmatrix3D& mStretch)
{
  std::vector<CEditModel::FrameGenerator> frames;

  // clears possible animations
  edm_md.CAnimData::Clear();
  edm_md.ad_NumberOfAnims = static_cast<INDEX>(animations.size());
  edm_md.ad_Anims = new COneAnim[animations.size()];
  size_t i = 0;
  for (const auto& anim : animations)
  {
    auto& one_anim = edm_md.ad_Anims[i++];
    strcpy(&one_anim.oa_Name[0], anim.m_name.substr(0, NAME_SIZE - 1).c_str());

    if (anim.m_type == ModelScript::Animation::Type::Skeletal)
    {
      auto importedAnimation = std::make_shared<ImportedSkeletalAnimation>(
        anim.m_frames.front(),
        anim.m_customSourceName.value_or(""),
        skeleton,
        anim.m_optNumFrames.value_or(0),
        anim.m_optDuration.value_or(0.0));

      if (anim.m_optRefSkeleton.has_value())
      {
        ImportedSkeleton refSkeleton;
        refSkeleton.FillFromFile(*anim.m_optRefSkeleton);
        importedAnimation->ReapplyByReference(refSkeleton);
      }

      one_anim.oa_NumberOfFrames = importedAnimation->m_frames.size();
      one_anim.oa_SecsPerFrame = importedAnimation->m_duration / importedAnimation->m_frames.size();
      one_anim.oa_FrameIndices = (INDEX*)AllocMemory(one_anim.oa_NumberOfFrames * sizeof(INDEX));

      for (size_t frameIndex = 0; frameIndex < importedAnimation->m_frames.size(); ++frameIndex)
      {
        one_anim.oa_FrameIndices[frameIndex] = frames.size();
        frames.emplace_back();
        auto& frame = frames.back();
        frame.m_filename = anim.m_frames.front();
        frame.m_generator = [&baseMesh, mStretch, importedAnimation, frameIndex](ImportedMesh& mesh)
        {
          mesh = baseMesh;
          const auto& animSkeleton = importedAnimation->m_frames[frameIndex];
          mesh.ApplySkinning(animSkeleton, mStretch);
        };
      }
    }
    else // vertex animation
    {
      one_anim.oa_NumberOfFrames = anim.m_frames.size();
      if (anim.m_optDuration.has_value())
        one_anim.oa_SecsPerFrame = (*anim.m_optDuration) / anim.m_frames.size();
      else
        one_anim.oa_SecsPerFrame = 0.1f;
      one_anim.oa_FrameIndices = (INDEX*)AllocMemory(one_anim.oa_NumberOfFrames * sizeof(INDEX));

      INDEX iFrame = 0;
      for (const auto& frame_filename : anim.m_frames)
      {
        // find existing index (of insert new one) for this file name into FileNameList
        auto foundFrame = std::find_if(frames.begin(), frames.end(),
          [&](const FrameGenerator& frame) { return frame.m_filename == frame_filename; });
        size_t frameIndex = frames.size();
        if (foundFrame != frames.end())
        {
          frameIndex = std::distance(frames.begin(), foundFrame);
        } else {
          frames.emplace_back();
          auto& frame = frames.back();
          frame.m_filename = frame_filename;
          frame.m_generator = [mStretch, frame_filename](ImportedMesh& mesh)
          {
            mesh.FillFromFile(frame_filename, mStretch);
          };
        }
        one_anim.oa_FrameIndices[iFrame++] = frameIndex;
      }
    }
  }
  return frames;
}

void CEditModel::LoadModelAnimationData_t(
  const ModelScript::Animations& animations,
  const ImportedMesh& baseMesh,
  const ImportedSkeleton& skeleton,
  const FLOATmatrix3D &mStretch)
{
  struct VertexNeighbors { CStaticStackArray<INDEX> vp_aiNeighbors; };

  FLOATaabbox3D OneFrameBB;
  FLOATaabbox3D AllFramesBB;

  INDEX ctFramesBefore = edm_md.md_FramesCt;
  edm_md.ClearAnimations();

  // there must be at least one mip model loaded, throw if not
  if( edm_md.md_VerticesCt == 0) {
    throw( "Trying to update model's animations, but model doesn't exists!");
  }
  auto frameGenerators = LoadFrameGenerators(animations, baseMesh, skeleton, mStretch);

  // if recreating animations, frame count must be the same
  if( (ctFramesBefore != 0) && (frameGenerators.size() != ctFramesBefore) )
  {
    throw( "If you are updating animations, you can't change number of frames. \
      If you want to add or remove some frames or animations, please recreate the model.");
  }
  edm_md.md_FramesCt = frameGenerators.size();

  /*
   * Now we will allocate frames and frames info array and array od 3D objects,
   * one for each frame.
   */

  if( ProgresRoutines.SetProgressMessage != NULL) {
    ProgresRoutines.SetProgressMessage( "Calculating bounding boxes ...");
  }
  if( ProgresRoutines.SetProgressRange != NULL) {
    ProgresRoutines.SetProgressRange(frameGenerators.size());
  }

  edm_md.md_FrameInfos.New( edm_md.md_FramesCt);
  if( edm_md.md_Flags & MF_COMPRESSED_16BIT) {
    edm_md.md_FrameVertices16.New( edm_md.md_FramesCt * edm_md.md_VerticesCt);
  } else {
    edm_md.md_FrameVertices8.New( edm_md.md_FramesCt * edm_md.md_VerticesCt);
  }

  INDEX iO3D = 0;                        // index used for progress dialog
  std::vector<FLOAT3D> avVertices; // for caching all vertices in all frames
  avVertices.reserve(baseMesh.m_vertices.size() * frameGenerators.size());

  for (auto& frameGenerator : frameGenerators)
  {
    if( ProgresRoutines.SetProgressState != NULL) ProgresRoutines.SetProgressState(iO3D);
    ImportedMesh mesh;
    frameGenerator.m_generator(mesh);

    if( edm_md.md_VerticesCt != mesh.m_vertices.size()) {
      ThrowF_t( "File %s, one of animation frame files has wrong number of points.", 
        frameGenerator.m_filename);
    }

    // normalize (clear) our Bounding Box
    OneFrameBB = FLOATaabbox3D();
    // Bounding Box makes union with all points in this frame
    for(INDEX i=0; i<edm_md.md_VerticesCt; i++) {
      FLOAT3D vVtx = mesh.m_vertices[i];
      OneFrameBB |= FLOATaabbox3D(vVtx);
      avVertices.emplace_back(vVtx);
    }
    // remember this frame's Bounding Box
    edm_md.md_FrameInfos[iO3D].mfi_Box = OneFrameBB;
    // make union with Bounding Box of all frames
    AllFramesBB |= OneFrameBB;
    // load next frame
    iO3D++;
  }

  // calculate stretch vector
  edm_md.md_Stretch = AllFramesBB.Size()/2.0f;       // get size of bounding box

  // correct invalid stretch factors
  if( edm_md.md_Stretch(1) == 0.0f) edm_md.md_Stretch(1) = 1.0f;
  if( edm_md.md_Stretch(2) == 0.0f) edm_md.md_Stretch(2) = 1.0f;
  if( edm_md.md_Stretch(3) == 0.0f) edm_md.md_Stretch(3) = 1.0f;

  // build links from vertices to polygons
  CStaticArray<VertexNeighbors> avnVertices;
  avnVertices.New( edm_md.md_VerticesCt);

  // loop thru polygons
  for (const auto& triangle : baseMesh.m_triangles)
  {
    INDEX iVtx0 = triangle.ct_iVtx[0];
    INDEX iVtx1 = triangle.ct_iVtx[1];
    INDEX iVtx2 = triangle.ct_iVtx[2];
    // add neighbor vertices for each of this vertices
    avnVertices[iVtx0].vp_aiNeighbors.Push() = iVtx2;
    avnVertices[iVtx0].vp_aiNeighbors.Push() = iVtx1;
    avnVertices[iVtx1].vp_aiNeighbors.Push() = iVtx0;
    avnVertices[iVtx1].vp_aiNeighbors.Push() = iVtx2;
    avnVertices[iVtx2].vp_aiNeighbors.Push() = iVtx1;
    avnVertices[iVtx2].vp_aiNeighbors.Push() = iVtx0;
  }
  // vertex->polygons links created

  // cache strecthing reciprocal for faster calc
  FLOAT f1oStretchX, f1oStretchY, f1oStretchZ;
  if( edm_md.md_Flags & MF_COMPRESSED_16BIT) {
    f1oStretchX = 32767.0f / edm_md.md_Stretch(1);
    f1oStretchY = 32767.0f / edm_md.md_Stretch(2);
    f1oStretchZ = 32767.0f / edm_md.md_Stretch(3);
  } else {
    f1oStretchX = 127.0f / edm_md.md_Stretch(1);
    f1oStretchY = 127.0f / edm_md.md_Stretch(2);
    f1oStretchZ = 127.0f / edm_md.md_Stretch(3);
  }

  // remember center vector
  FLOAT3D vCenter = AllFramesBB.Center();  // obtain bbox center
  edm_md.md_vCenter = vCenter;

  // prepare progress bar
  if( ProgresRoutines.SetProgressMessage != NULL) {
    ProgresRoutines.SetProgressMessage( "Calculating gouraud normals and stretching vertices ...");
  }
  if( ProgresRoutines.SetProgressRange != NULL) {
    ProgresRoutines.SetProgressRange( edm_md.md_FramesCt);
  }

  // loop thru frames
  iO3D=0;  // index for progress
  INDEX iFVtx=0;  // count for all vertices in all frames
  for( INDEX iFr=0; iFr<edm_md.md_FramesCt; iFr++)
  {
    // calculate all polygon normals for this frame
    if( ProgresRoutines.SetProgressState != NULL) ProgresRoutines.SetProgressState(iO3D);
    for( INDEX iVtx=0; iVtx<edm_md.md_VerticesCt; iVtx++)  // for all vertices in one frame
    {
      FLOAT3D &vVtx = avVertices[iFVtx];
      if( edm_md.md_Flags & MF_COMPRESSED_16BIT) {
        edm_md.md_FrameVertices16[iFVtx].mfv_SWPoint = SWPOINT3D(
          FloatToInt( (vVtx(1) - vCenter(1)) * f1oStretchX),
          FloatToInt( (vVtx(2) - vCenter(2)) * f1oStretchY),
          FloatToInt( (vVtx(3) - vCenter(3)) * f1oStretchZ) );
      } else {                                                              
        edm_md.md_FrameVertices8[iFVtx].mfv_SBPoint = SBPOINT3D(           
          FloatToInt( (vVtx(1) - vCenter(1)) * f1oStretchX),
          FloatToInt( (vVtx(2) - vCenter(2)) * f1oStretchY),
          FloatToInt( (vVtx(3) - vCenter(3)) * f1oStretchZ) );
      }

      // calculate vector of gouraud normal in this vertice
      ANGLE   aSum = 0;
      FLOAT3D vSum( 0.0f, 0.0f, 0.0f);
      INDEX iFrOffset = edm_md.md_VerticesCt * iFr;
      VertexNeighbors &vnCurr = avnVertices[iVtx];
      for( INDEX iNVtx=0; iNVtx<vnCurr.vp_aiNeighbors.Count(); iNVtx+=2) { // loop thru neighbors
        INDEX iPrev = vnCurr.vp_aiNeighbors[iNVtx+0];
        INDEX iNext = vnCurr.vp_aiNeighbors[iNVtx+1];
        FLOAT3D v0  = avVertices[iPrev+iFrOffset] - vVtx;
        FLOAT3D v1  = avVertices[iNext+iFrOffset] - vVtx;
        v0.Normalize();
        v1.Normalize();
        FLOAT3D v = v1*v0;
        FLOAT fLength = v.Length();
        ANGLE a = ASin(fLength);
        //ASSERT( a>=0 && a<=180);
        aSum += a;
        vSum += (v/fLength) * a;
      }

      // normalize sum of polygon normals
      //ASSERT( aSum>=0);
      vSum /= aSum;
      vSum.Normalize();

      // save compressed gouraud normal
      if( edm_md.md_Flags & MF_COMPRESSED_16BIT) {
        CompressNormal_HQ( vSum, edm_md.md_FrameVertices16[iFVtx].mfv_ubNormH,
                                 edm_md.md_FrameVertices16[iFVtx].mfv_ubNormP);
      } else {
        edm_md.md_FrameVertices8[iFVtx].mfv_NormIndex = (UBYTE)GouraudNormal(vSum);
      }

      // advance to next vertex in model
      iFVtx++;
    }

    // advance to next frame
    iO3D++;
  }

  // create compressed vector center that will be used for setting object handle
  edm_md.md_vCompressedCenter(1) = -edm_md.md_vCenter(1) * f1oStretchX;
  edm_md.md_vCompressedCenter(2) = -edm_md.md_vCenter(2) * f1oStretchY;
  edm_md.md_vCompressedCenter(3) = -edm_md.md_vCenter(3) * f1oStretchZ;

  // adjust stretching for compressed format
  if( edm_md.md_Flags & MF_COMPRESSED_16BIT) {
    edm_md.md_Stretch(1) /= 32767.0f; 
    edm_md.md_Stretch(2) /= 32767.0f;
    edm_md.md_Stretch(3) /= 32767.0f;
  } else {
    edm_md.md_Stretch(1) /= 127.0f; 
    edm_md.md_Stretch(2) /= 127.0f;
    edm_md.md_Stretch(3) /= 127.0f;
  }
}


//--------------------------------------------------------------------------------------------
/*
 * Routine saves model's .h file (#define ......)
 */
void CEditModel::SaveIncludeFile_t( CTFileName fnFileName, CTString strDefinePrefix) // throw char *
{
  CTFileStream strmHFile;
  char line[ 1024];
  INDEX i;

  strmHFile.Create_t( fnFileName, CTStream::CM_TEXT);
  strcpy( line, strDefinePrefix);
  strupr( line);
  strDefinePrefix = CTString( line);

  sprintf( line, "// Animation names\n");
  strmHFile.Write_t( line, strlen( line));
  // force animation prefix string to be upper case
  char achrUprName[ 256];
  strcpy( achrUprName, strDefinePrefix);
  strcat( achrUprName, "_ANIM_");
  CTString strAnimationPrefix = achrUprName;
  ExportAnimationNames_t(edm_md, &strmHFile, achrUprName);

  sprintf( line, "\n// Color names\n");
  strmHFile.Write_t( line, strlen( line));

  for( i=0; i<MAX_COLOR_NAMES; i++)
  {
    if( edm_md.md_ColorNames[ i] != "")
    {
      sprintf( line, "#define %s_PART_%s ((1L) << %d)\n", strDefinePrefix, edm_md.md_ColorNames[ i], i);
      strmHFile.Write_t( line, strlen( line));
    }
  }
  sprintf( line, "\n// Patch names\n");
  strmHFile.Write_t( line, strlen( line));

  for( INDEX iPatch=0; iPatch<MAX_TEXTUREPATCHES; iPatch++)
  {
    CTString strPatchName = edm_md.md_mpPatches[ iPatch].mp_strName;
    if( strPatchName != "")
    {
      sprintf( line, "#define %s_PATCH_%s %d\n", strDefinePrefix, strPatchName, i);
      strmHFile.Write_t( line, strlen( line));
    }
  }
  // save collision box names
  sprintf( line, "\n// Names of collision boxes\n");
  strmHFile.Write_t( line, strlen( line));

  edm_md.md_acbCollisionBox.Lock();
  // save all collision boxes
  for( INDEX iCollisionBox=0; iCollisionBox<edm_md.md_acbCollisionBox.Count(); iCollisionBox++)
  {
    // prepare collision box name as define
    sprintf( line, "#define %s_COLLISION_BOX_%s %d\n", strDefinePrefix, GetCollisionBoxName( iCollisionBox),
      iCollisionBox);
    strmHFile.Write_t( line, strlen( line));
  }
  edm_md.md_acbCollisionBox.Unlock();

  // save all attaching positions
  sprintf( line, "\n// Attaching position names\n");
  strmHFile.Write_t( line, strlen( line));
  INDEX iAttachingPlcement = 0;
  FOREACHINDYNAMICARRAY(edm_aamAttachedModels, CAttachedModel, itam)
  {
    char achrUpper[ 256];
    strcpy( achrUpper, itam->am_strName);
    strupr( achrUpper);
    sprintf( line, "#define %s_ATTACHMENT_%s %d\n", strDefinePrefix, achrUpper, iAttachingPlcement);
    strmHFile.Write_t( line, strlen( line));
    iAttachingPlcement++;
  }
  sprintf( line, "\n// Sound names\n");
  strmHFile.Write_t( line, strlen( line));

  for( INDEX iSound=0; iSound<edm_aasAttachedSounds.Count(); iSound++)
  {
    if( edm_aasAttachedSounds[iSound].as_fnAttachedSound != "")
    {
      CTString strLooping;
      if( edm_aasAttachedSounds[iSound].as_bLooping) strLooping = "L";
      else                                           strLooping = "NL";

      CTString strDelay = "";
      if( edm_aasAttachedSounds[iSound].as_fDelay == 0.0f)
        strDelay = "0.0f";
      else
        strDelay.PrintF( "%gf", edm_aasAttachedSounds[iSound].as_fDelay);

      CAnimInfo aiInfo;
      edm_md.GetAnimInfo( iSound, aiInfo);

      CTString strWithQuotes;
      strWithQuotes.PrintF( "\"%s\",", CTString(edm_aasAttachedSounds[iSound].as_fnAttachedSound));

      sprintf( line, "//sound SOUND_%s_%-16s %-32s // %s, %s, %s\n",
        strDefinePrefix,
        aiInfo.ai_AnimName,
        strWithQuotes,
        strAnimationPrefix+aiInfo.ai_AnimName,
        strLooping,
        strDelay);
      strmHFile.Write_t( line, strlen( line));
    }
  }

  strmHFile.Close();
}

// overloaded save function
void CEditModel::Save_t( CTFileName fnFileName) // throw char *
{
  CTFileName fnMdlFileName = fnFileName.FileDir() + fnFileName.FileName() + ".mdl";
  edm_md.Save_t( fnMdlFileName);

  CTFileName fnHFileName = fnFileName.FileDir() + fnFileName.FileName() + ".h";
  CTString strPrefix = fnFileName.FileName();
  if (strPrefix.Length()>0 && !isalpha(strPrefix[0]) && strPrefix[0]!='_') {
    strPrefix="_"+strPrefix;
  }
  SaveIncludeFile_t( fnHFileName, strPrefix);

  CTFileName fnIniFileName = fnFileName.FileDir() + fnFileName.FileName() + ".ini";
  CSerial::Save_t( fnIniFileName);
}

// overloaded load function
void CEditModel::Load_t( CTFileName fnFileName)
{
  CTFileName fnMdlFileName = fnFileName.FileDir() + fnFileName.FileName() + ".mdl";
  edm_md.Load_t( fnMdlFileName);

  CTFileName fnIniFileName = fnFileName.FileDir() + fnFileName.FileName() + ".ini";
  // try to load ini file
  try
  {
    CSerial::Load_t( fnIniFileName);
  }
  catch(char *strError)
  {
    // ignore errors
    (void) strError;
    CreateEmptyAttachingSounds();
  }
}

CTextureDataInfo *CEditModel::AddTexture_t(const CTFileName &fnFileName, const MEX mexWidth,
                            const MEX mexHeight)
{
  CTextureDataInfo *pNewTDI = new CTextureDataInfo;
  pNewTDI->tdi_FileName = fnFileName;

  try
  {
    pNewTDI->tdi_TextureData = _pTextureStock->Obtain_t( pNewTDI->tdi_FileName);
  }
  catch(char *strError)
  {
    (void) strError;
    delete pNewTDI;
    return NULL;
  }

  // reload the texture
  pNewTDI->tdi_TextureData->Reload();

  edm_WorkingSkins.AddTail( pNewTDI->tdi_ListNode);
  return pNewTDI;
}

CAttachedModel::CAttachedModel(void)
{
  am_strName = "No name";
  am_iAnimation = 0;
  am_bVisible = TRUE;
}

CAttachedModel::~CAttachedModel(void)
{
  Clear();
}

void CAttachedModel::Clear(void)
{
  am_moAttachedModel.mo_toTexture.SetData(NULL);
  am_moAttachedModel.mo_toReflection.SetData(NULL);
  am_moAttachedModel.mo_toSpecular.SetData(NULL);
  am_moAttachedModel.mo_toBump.SetData(NULL);
  am_moAttachedModel.SetData(NULL);
}

void CAttachedModel::Read_t( CTStream *pstrmFile) // throw char *
{
  *pstrmFile >> am_bVisible;
  *pstrmFile >> am_strName;
  // this data is used no more
  CTFileName fnModel, fnDummy;
  *pstrmFile >> fnModel;
  
  // new attached model format has saved index of animation
  if( pstrmFile->PeekID_t() == CChunkID("AMAN"))
  {
    pstrmFile->ExpectID_t( CChunkID( "AMAN"));
    *pstrmFile >> am_iAnimation;
  }
  else
  {
    *pstrmFile >> fnDummy; // ex model's texture
  }

  try
  {
    SetModel_t( fnModel);
  }
  catch(char *strError)
  {
    (void) strError;
    try
    {
      SetModel_t( CTFILENAME("Models\\Editor\\Axis.mdl"));
    }
    catch(char *strError)
    {
      FatalError( strError);
    }
  }
}

void CAttachedModel::Write_t( CTStream *pstrmFile) // throw char *
{
  *pstrmFile << am_bVisible;
  *pstrmFile << am_strName;
  *pstrmFile << am_moAttachedModel.GetName();

  // new attached model format has saved index of animation
  pstrmFile->WriteID_t( CChunkID("AMAN"));
  *pstrmFile << am_iAnimation;
}

void CAttachedModel::SetModel_t(CTFileName fnModel)
{
  am_moAttachedModel.SetData_t(fnModel);
  am_moAttachedModel.AutoSetTextures();
}

CAttachedSound::CAttachedSound( void)
{
  as_fDelay = 0.0f;
  as_fnAttachedSound = CTString("");
  as_bLooping = FALSE;
  as_bPlaying = TRUE;
}

void CAttachedSound::Read_t(CTStream *strFile)
{
  *strFile>>as_bLooping;
  *strFile>>as_bPlaying;
  *strFile>>as_fnAttachedSound;
  *strFile>>as_fDelay;
}

void CAttachedSound::Write_t(CTStream *strFile)
{
  *strFile<<as_bLooping;
  *strFile<<as_bPlaying;
  *strFile<<as_fnAttachedSound;
  *strFile<<as_fDelay;
}

void CEditModel::CreateEmptyAttachingSounds( void)
{
  ASSERT( edm_md.GetAnimsCt() > 0);
  edm_aasAttachedSounds.Clear();
  edm_aasAttachedSounds.New( edm_md.GetAnimsCt());
}

void CEditModel::Read_t( CTStream *pFile) // throw char *
{
  CTFileName fnFileName;
  INDEX i, iWorkingTexturesCt;

  pFile->ExpectID_t( CChunkID( "WTEX"));
  *pFile >> iWorkingTexturesCt;

  for( i=0; i<iWorkingTexturesCt; i++)
  {
    *pFile >> fnFileName;
    try
    {
      AddTexture_t( fnFileName, edm_md.md_Width, edm_md.md_Height);
    }
    // This is here because we want to load model even if its texture is not valid
    catch( char *err_str){ (char *) err_str;}
  }

  // skip patches saved in old format (patches do not exist inside EditModel any more)
  if( pFile->PeekID_t() == CChunkID("PATM"))
  {
    pFile->GetID_t();
    ULONG ulDummySizeOfLong;
    ULONG ulOldExistingPatches;

    *pFile >> ulDummySizeOfLong;
    *pFile >> ulOldExistingPatches;
    for( i=0; i<MAX_TEXTUREPATCHES; i++)
    {
      if( ((1UL << i) & ulOldExistingPatches) != 0)
      {
        CTFileName fnPatchName;
        *pFile >> fnPatchName;
      }
    }
  }

  // try to load attached models
  try
  {
    pFile->ExpectID_t( CChunkID( "ATTM"));
    INDEX ctSavedModels;
    *pFile >> ctSavedModels;

    // clamp no of saved attachments to no of model's data attached positions
    INDEX ctMDAttachments = edm_md.md_aampAttachedPosition.Count();
    INDEX ctToLoad = ClampUp( ctSavedModels, ctMDAttachments);
    INDEX ctToSkip = ctSavedModels - ctToLoad;

    // add attached models
    edm_aamAttachedModels.Clear();
    if( ctToLoad != 0)
    {
      edm_aamAttachedModels.New( ctSavedModels);
      // read all attached models
      FOREACHINDYNAMICARRAY(edm_aamAttachedModels, CAttachedModel, itam)
      {
        itam->Read_t(pFile);
      }
    }

    // skip unused attached models
    for( INDEX iSkip=0; iSkip<ctToSkip; iSkip++)
    {
      CAttachedModel atmDummy;
      atmDummy.Read_t(pFile);
    }
  }
  catch( char *strError)
  {
    (void) strError;
    // clear attached models
    edm_aamAttachedModels.Clear();
    edm_md.md_aampAttachedPosition.Clear();
  }

  CreateEmptyAttachingSounds();
  // try to load attached sounds
  try
  {
    pFile->ExpectID_t( CChunkID( "ATSD"));
    INDEX ctAttachedSounds;
    *pFile >> ctAttachedSounds;
    INDEX ctExisting = edm_aasAttachedSounds.Count();
    INDEX ctToRead = ClampUp( ctAttachedSounds, ctExisting);

    // read all saved attached sounds
    for( INDEX iSound=0; iSound<ctToRead; iSound++)
    {
      CAttachedSound &as = edm_aasAttachedSounds[ iSound];
      as.Read_t(pFile);
    }

    // skipped saved but now obsolite
    INDEX ctToSkip = ctAttachedSounds - ctToRead;
    for( INDEX iSkip=0; iSkip<ctToSkip; iSkip++)
    {
      CAttachedSound asDummy;
      asDummy.Read_t(pFile);
    }
  }
  catch( char *strError)
  {
    (void) strError;
  }

  try
  {
    // load last taken thumbnail settings
    pFile->ExpectID_t( CChunkID( "TBST"));
    edm_tsThumbnailSettings.Read_t( pFile);
  }
  catch( char *strError)
  {
    // ignore errors
    (void) strError;
  }

  // load names of effect textures
  // --- specular texture
  try {
    pFile->ExpectID_t( CChunkID( "FXTS"));
    *pFile >> edm_fnSpecularTexture;
  } catch( char *strError) { (void) strError; }

  // --- reflection texture
  try {
    pFile->ExpectID_t( CChunkID( "FXTR"));
    *pFile >> edm_fnReflectionTexture;
  } catch( char *strError) { (void) strError; }

  // --- bump texture
  try {
    pFile->ExpectID_t( CChunkID( "FXTB"));
    *pFile >> edm_fnBumpTexture;
  } catch( char *strError) { (void) strError; }
}

void CEditModel::Write_t( CTStream *pFile) // throw char *
{
  pFile->WriteID_t( CChunkID( "WTEX"));

  INDEX iWorkingTexturesCt = edm_WorkingSkins.Count();
  *pFile << iWorkingTexturesCt;

  FOREACHINLIST( CTextureDataInfo, tdi_ListNode, edm_WorkingSkins, it)
  {
    *pFile << it->tdi_FileName;
  }

  // CEditModel class has no patches in new patch data format

  pFile->WriteID_t( CChunkID( "ATTM"));
  INDEX ctAttachedModels = edm_aamAttachedModels.Count();
  *pFile << ctAttachedModels;
  // write all attached models
  FOREACHINDYNAMICARRAY(edm_aamAttachedModels, CAttachedModel, itam)
  {
    itam->Write_t(pFile);
  }

  pFile->WriteID_t( CChunkID( "ATSD"));
  INDEX ctAttachedSounds = edm_aasAttachedSounds.Count();
  *pFile << ctAttachedSounds;
  // write all attached models
  FOREACHINSTATICARRAY(edm_aasAttachedSounds, CAttachedSound, itas)
  {
    itas->Write_t(pFile);
  }

  // save last taken thumbnail settings
  pFile->WriteID_t( CChunkID( "TBST"));
  edm_tsThumbnailSettings.Write_t( pFile);

  // save names of effect textures
  // --- specular texture
  pFile->WriteID_t( CChunkID( "FXTS"));
  *pFile << edm_fnSpecularTexture;
  // --- reflection texture
  pFile->WriteID_t( CChunkID( "FXTR"));
  *pFile << edm_fnReflectionTexture;
  // --- bump texture
  pFile->WriteID_t( CChunkID( "FXTB"));
  *pFile << edm_fnBumpTexture;
}
//----------------------------------------------------------------------------------------------
/*
 * Routine saves default script file containing only one animation with default data
 */
void CEditModel::CreateScriptFile_t(CTFileName &fnO3D) // throw char *
{
  ModelScript script;
  script.m_mipModels.push_back(fnO3D);

  const auto skel_anims = ImportedSkeletalAnimation::GetAnimationsInFile(fnO3D);
  if (ImportedSkeleton::ContainsSkeleton(fnO3D) && !skel_anims.empty())
  {
    for (const auto& anim_name : skel_anims)
    {
      script.m_animations.emplace_back();
      auto& anim = script.m_animations.back();
      anim.m_type = ModelScript::Animation::Type::Skeletal;
      anim.m_name = anim_name;
      if (skel_anims.size() > 1)
        anim.m_customSourceName = anim_name;
      anim.m_frames.push_back(fnO3D);
    }
  }
  else
  {
    script.m_animations.emplace_back();
    auto& anim = script.m_animations.back();
    anim.m_type = ModelScript::Animation::Type::Vertex;
    anim.m_name = "Default";
    anim.m_frames.push_back(fnO3D);
  }

  const CTFileName fnScriptName = fnO3D.FileDir() + fnO3D.FileName() + ".scr";
  ScriptIO::SaveToFile(script, fnScriptName);
}
//----------------------------------------------------------------------------------------------
/*
 * This routine load lines from script file and executes appropriate actions
 */
void CEditModel::LoadFromScript_t(CTFileName &fnScriptName) // throw char *
{
  const auto script = ScriptIO::ReadFromFile(fnScriptName);

  if (script.m_flat == ModelScript::Flat::Half)
    edm_md.md_Flags |= MF_FACE_FORWARD | MF_HALF_FACE_FORWARD;
  else if (script.m_flat == ModelScript::Flat::Full)
    edm_md.md_Flags |= MF_FACE_FORWARD;

  if (script.m_stretchDetail)
    edm_md.md_Flags |= MF_STRETCH_DETAIL;

  if (script.m_highQuality)
    edm_md.md_Flags |= MF_COMPRESSED_16BIT;

  edm_md.md_ShadowQuality = script.m_maxShadow;

  edm_md.md_Width = MEX_METERS(script.m_textureScale(1));
  edm_md.md_Height = MEX_METERS(script.m_textureScale(2));

  const FLOATmatrix3D mStretch = script.m_transformation * script.m_scale;

  const bool allowedToCreateBoneTriangles =
    script.m_boneTriangles &&
    std::all_of(script.m_animations.begin(), script.m_animations.end(),
      [](const ModelScript::Animation& a)
      { return a.m_type == ModelScript::Animation::Type::Skeletal; });

  ImportedSkeleton skeleton;
  if (script.m_skeleton.has_value())
    skeleton.FillFromFile(*script.m_skeleton);
  else
    skeleton.FillFromFile(script.m_mipModels.front());

  ImportedMesh baseMesh;
  for (const auto& mip_file : script.m_mipModels)
  {
    ImportedMesh mesh(mip_file, mStretch);
    if (baseMesh.m_vertices.empty())
    {
      if (allowedToCreateBoneTriangles)
        CreateBoneTriangles(mesh, mStretch, script.m_scale);
      baseMesh = mesh;
    }
    if (edm_md.md_VerticesCt == 0)
      NewModel(mesh);
    else
      AddMipModel(mesh);
  }
  edm_md.SpreadMipSwitchFactors(0, 5.0f);

  LoadModelAnimationData_t(script.m_animations, baseMesh, skeleton, mStretch);

  edm_md.md_acbCollisionBox.New();
  CreateEmptyAttachingSounds();
  edm_md.LinkDataForSurfaces(TRUE);

  // try to
  try
  {
    // load mapping
    LoadMapping_t( CTString(fnScriptName.NoExt()+".map"));
  }
  // if not successful
  catch (char *strError)
  {
    // ignore error message
    (void)strError;
  }

  if (edm_aasAttachedSounds.Count() == 0)
    CreateEmptyAttachingSounds();
}

//----------------------------------------------------------------------------------------------
/*
 * Routine takes Object 3D class as input and creates new model (model data)
 * with its polygons, vertices, surfaces
 */
void CEditModel::NewModel(const ImportedMesh& mesh)
{
  edm_md.md_VerticesCt = mesh.m_vertices.size();  // see how many vertices we will have
  edm_md.md_TransformedVertices.New( edm_md.md_VerticesCt); // create buffer for rotated vertices
  edm_md.md_MainMipVertices.New( edm_md.md_VerticesCt); // create buffer for main mip vertices
  edm_md.md_VertexMipMask.New( edm_md.md_VerticesCt);  // create buffer for vertex masks

  for( INDEX i=0; i<edm_md.md_VerticesCt; i++)
  {
    // copy vertex coordinates into md_MainMipVertices array so we colud make
    // mip-models later (we will search original coordinates in this array)
    edm_md.md_MainMipVertices[ i] = mesh.m_vertices[i];
    edm_md.md_VertexMipMask[i] = 0L; // mark to all vertices that they don't exist in any mip-model
  }

  AddMipModel(mesh);                 // we add main model, first mip-model
}

//----------------------------------------------------------------------------------------------
/*
 * Routine takes 3D object as input and adds one mip model
 * The main idea is: for every vertice get distances to all vertices in md_MainMipVertices
 * array. If minimum distance is found, set that this vertice exists. Loop for all vertices.
 * Throw error if minimum distance isn't found. Set also new mip-model polygons info.
 */
void CEditModel::AddMipModel(const ImportedMesh& mesh)
{
  INDEX i, j;

  // this is mask for vertices in current mip level
  ULONG mip_vtx_mask = (1L) << edm_md.md_MipCt;

  struct ModelMipInfo *pmmpi = &edm_md.md_MipInfos[ edm_md.md_MipCt]; // point to mip model that we will create

  // for each vertex
  for( INDEX iVertex=0; iVertex<edm_md.md_VerticesCt; iVertex++)
  {
    // mark that it is not in this mip model
    edm_md.md_VertexMipMask[ iVertex] &= ~mip_vtx_mask;
  }

  INDEX o3dvct = mesh.m_vertices.size();
  /*
   * For each vertex in 3D object we calculate distances to all vertices in main mip-model.
   * If distance (size of vector that is result of substraction of two vertice vectors) is
   * less than some minimal float number, we assume that these vertices are the same.
   * Processed vertex of 3D object gets its main-mip-model-vertex-friend's index as tag and
   * mask value showing that it exists in this mip-model.
   */
  std::vector<INDEX> verticesRemap(mesh.m_vertices.size(), 0);
  for( i=0; i<o3dvct; i++)
  {
    INDEX same_index = -1;

    if (edm_md.md_MipCt == 0)
    {
      same_index = i;
    }
    else
    {
      for (j = 0; j < edm_md.md_VerticesCt; j++)
      {
        FLOAT3D vVertex = mesh.m_vertices[i];
        FLOAT fAbsoluteDistance = Abs((vVertex - edm_md.md_MainMipVertices[j]).Length());
        if (fAbsoluteDistance < MAX_ALLOWED_DISTANCE)
        {
          same_index = j; // mark that this vertex's remap is found
          break;
        }
      }
    }

    if (same_index == -1)  // if no vertice close enough is found, we have error
    {
      ThrowF_t("Vertex from mip model %d with number %d, coordinates (%f,%f,%f), can't be found in main mip model.\n"
        "There can't be new vertices in rougher mip-models,"
        "but only vertices from main mip model can be removed and polygons reorganized.\n",
        edm_md.md_MipCt, i,
        mesh.m_vertices[i](1), mesh.m_vertices[i](2), mesh.m_vertices[i](3));
    }

    edm_md.md_VertexMipMask[same_index] |= mip_vtx_mask; // we mark that this vertice exists in this mip model
    verticesRemap[i] = same_index; // remapping verice index must be remembered
  }

  /*
   * We will create three arays for this mip polygon info:
   *  1) array for polygons
   *  2) array for mapping surfaces
   *  3) array for polygon vertices
   *  4) array for texture vertices
   */

  /*
   * First we create array large enough to accept object 3D's polygons.
   */
  pmmpi->mmpi_PolygonsCt = mesh.m_triangles.size();
  pmmpi->mmpi_Polygons.New( pmmpi->mmpi_PolygonsCt);

  /*
   * Then we will create array for mapping surfaces and set their names
   */
  pmmpi->mmpi_MappingSurfaces.New(mesh.m_materials.size());  // create array for mapping surfaces
  for( i=0; i<mesh.m_materials.size(); i++)
  {
    MappingSurface &ms = pmmpi->mmpi_MappingSurfaces[ i];
    ms.ms_ulOnColor = PC_ALLWAYS_ON;              // set default ON and OFF masking colors
    ms.ms_ulOffColor = PC_ALLWAYS_OFF;
    ms.ms_Name = CTFileName( mesh.m_materials[i].cm_strName);

    ms.ms_colColor =
      mesh.m_materials[i].cm_colColor | CT_OPAQUE; // copy surface color, set no alpha
    ms.ms_sstShadingType = SST_MATTE;
    ms.ms_sttTranslucencyType = STT_OPAQUE;
    ms.ms_ulRenderingFlags = SRF_DIFFUSE|SRF_NEW_TEXTURE_FORMAT;
  }


  struct VertexRemap
  {
    FLOAT2D uv;
    INDEX material;
    INDEX global;

    bool operator == (const VertexRemap& other) const
    {
      return uv == other.uv && material == other.material && global == other.global;
    }

    struct Hasher
    {
      size_t operator()(const VertexRemap& v) const
      {
        size_t result = FLOAT2D::Hasher()(v.uv);
        HashCombine(result, v.material);
        HashCombine(result, v.global);
        return result;
      }
    };
  };

  std::unordered_map<VertexRemap, INDEX, VertexRemap::Hasher> uniqueTexCoords;
  std::vector<FLOAT2D> orderedUniqueTexCoords;
  std::vector<INDEX> texCoordsRemap;
  for (const auto& triangle : mesh.m_triangles)
  {
    for (size_t e = 0; e < 3; ++e)
    {
      FLOAT2D vtxUV(mesh.m_uvs[0][triangle.ct_iTVtx[0][e]]);

      VertexRemap vertex_remap{ vtxUV, triangle.ct_iMaterial, triangle.ct_iVtx[e] };
      auto found_pos = uniqueTexCoords.find(vertex_remap);
      if (found_pos == uniqueTexCoords.end())
      {
        uniqueTexCoords[vertex_remap] = orderedUniqueTexCoords.size();
        texCoordsRemap.push_back(orderedUniqueTexCoords.size());
        orderedUniqueTexCoords.push_back(vtxUV);
      }
      else
      {
        texCoordsRemap.push_back(found_pos->second);
      }
    }
  }

  pmmpi->mmpi_TextureVertices.New(orderedUniqueTexCoords.size());

  for (size_t i = 0; i < orderedUniqueTexCoords.size(); ++i)
  {
    FLOAT2D uv_coord = orderedUniqueTexCoords[i];

    uv_coord(1) *= edm_md.md_Width / 1024.0f;
    uv_coord(2) *= edm_md.md_Height / 1024.0f;

    pmmpi->mmpi_TextureVertices[i].mtv_UVW = FLOAT3D(uv_coord(1), uv_coord(2), 0.0f);
    MEX2D mexUV;
    mexUV(1) = MEX_METERS(pmmpi->mmpi_TextureVertices[i].mtv_UVW(1));
    mexUV(2) = MEX_METERS(pmmpi->mmpi_TextureVertices[i].mtv_UVW(2));
    pmmpi->mmpi_TextureVertices[i].mtv_UV = mexUV;
  }

  /*
   * Now we intend to create data for all polygons (that includes setting polygon's
   * texture and transformed vertex ptrs)
   */
  INDEX mpvct = 0;
  for (i = 0; i < pmmpi->mmpi_PolygonsCt; i++)        // loop all model polygons
  {
    const auto& triangle = mesh.m_triangles[i];
    struct ModelPolygon* pmp = &pmmpi->mmpi_Polygons[i];    // ptr to activ model polygon
    pmp->mp_Surface = triangle.ct_iMaterial; // copy surface index
    pmp->mp_ColorAndAlpha =
      mesh.m_materials[triangle.ct_iMaterial].cm_colColor | CT_OPAQUE; // copy surface color, set no alpha

    pmp->mp_PolygonVertices.New(3); // create array for them
    for (j = 0; j < 3; j++)        // fill data for this polygon's vertices
    {
      /*
       * Here we really remap one mip models's vertex in a way that we set its transformed
       * vertex ptr after remapping it using link (tag) to its original mip-model's vertex
       */
      INDEX o3d_vertex = triangle.ct_iVtx[j];

      pmp->mp_PolygonVertices[j].mpv_ptvTransformedVertex =
        &edm_md.md_TransformedVertices[verticesRemap[o3d_vertex]];

      pmp->mp_PolygonVertices[j].mpv_ptvTextureVertex =
        &pmmpi->mmpi_TextureVertices[texCoordsRemap[mpvct++]];
    }
  }

  edm_md.md_MipCt ++;  // finally, this mip-model is done.
}

/*
 * This routine opens last script file loaded, repeats reading key-words until it finds
 * key-word "ANIM_START". Then it calls animation data load from script routine.
 */
void CEditModel::UpdateAnimations_t(CTFileName &fnScriptName) // throw char *
{
  const auto script = ScriptIO::ReadFromFile(fnScriptName);

  const FLOATmatrix3D mStretch = script.m_transformation * script.m_scale;

  const bool allowedToCreateBoneTriangles =
    script.m_boneTriangles &&
    std::all_of(script.m_animations.begin(), script.m_animations.end(),
      [](const ModelScript::Animation& a)
      {
        return a.m_type == ModelScript::Animation::Type::Skeletal;
      });

  ImportedSkeleton skeleton;
  if (script.m_skeleton.has_value())
    skeleton.FillFromFile(*script.m_skeleton);
  else
    skeleton.FillFromFile(script.m_mipModels.front());

  ImportedMesh baseMesh(script.m_mipModels.front(), mStretch);
  if (allowedToCreateBoneTriangles)
    CreateBoneTriangles(baseMesh, mStretch, script.m_scale);

  LoadModelAnimationData_t(script.m_animations, baseMesh, skeleton, mStretch);

  CreateEmptyAttachingSounds();
}
//----------------------------------------------------------------------------------------------
void CEditModel::CreateMipModels_t(const ImportedMesh& baseMesh, INDEX iVertexRemoveRate, INDEX iSurfacePreservingFactor)
{
  // free possible mip-models except main mip model
  INDEX iMipModel=1;
  for( ; iMipModel<edm_md.md_MipCt; iMipModel++)
  {
    edm_md.md_MipInfos[ iMipModel].Clear();
  }
  edm_md.md_MipCt = 1;

  // create mip model structure
  CMipModel mmMipModel(baseMesh);

  if( ProgresRoutines.SetProgressMessage != NULL)
    ProgresRoutines.SetProgressMessage( "Calculating mip models ...");
  INDEX ctVerticesInRestFrame = mmMipModel.mm_amvVertices.Count();
  if( ProgresRoutines.SetProgressRange != NULL)
    ProgresRoutines.SetProgressRange(ctVerticesInRestFrame);
  // create maximum 32 mip models
  for( iMipModel=0; iMipModel<31; iMipModel++)
  {
    // if unable to create mip models
    if( !mmMipModel.CreateMipModel_t( iVertexRemoveRate, iSurfacePreservingFactor))
    {
      // stop creating more mip models
      break;
    }
    if( ProgresRoutines.SetProgressState != NULL)
      ProgresRoutines.SetProgressState(ctVerticesInRestFrame - mmMipModel.mm_amvVertices.Count());
    AddMipModel(mmMipModel.GetMesh());
  }
  ProgresRoutines.SetProgressState(ctVerticesInRestFrame);
  edm_md.SpreadMipSwitchFactors( 0, 5.0f);
  edm_md.LinkDataForSurfaces(FALSE);
}
//----------------------------------------------------------------------------------------------
/*
 * This routine discards all mip-models except main (mip-model 0). Then it opens script file
 * with file name of last script loaded. Then it repeats reading key-words until it counts two
 * key-words "MIPMODEL". For second and all other key-words routine calls add mip-map routine
 */
void CEditModel::UpdateMipModels_t(CTFileName &fnScriptName) // throw char *
{
  const auto script = ScriptIO::ReadFromFile(fnScriptName);

  const FLOATmatrix3D mStretch = script.m_transformation * script.m_scale;
  ASSERT( edm_md.md_VerticesCt != 0);

  // free possible mip-models except main mip model
  for (INDEX i = 1; i<edm_md.md_MipCt; i++)
    edm_md.md_MipInfos[i].Clear();
  edm_md.md_MipCt = 1;

  for (size_t i = 1; i < script.m_mipModels.size(); ++i)
  {
    const auto& mip = script.m_mipModels.at(i);
    ImportedMesh mesh(mip, mStretch);
    if (edm_md.md_VerticesCt < mesh.m_vertices.size())
      ThrowF_t(
        "It is unlikely that mip-model \"%s\" is valid.\n"
        "It contains more vertices than main mip-model so it can't be mip-model.",
        mip);
    AddMipModel(mesh);
  }

  edm_md.LinkDataForSurfaces(TRUE);
}

/*
 * Draws given surface in wire frame
 */
void CEditModel::DrawWireSurface( CDrawPort *pDP, INDEX iCurrentMip, INDEX iCurrentSurface,
                                  FLOAT fMagnifyFactor, PIX offx, PIX offy,
                                  COLOR clrVisible, COLOR clrInvisible)
{
  FLOAT3D f3dTr0, f3dTr1, f3dTr2;
  struct ModelTextureVertex *pVtx0, *pVtx1;

  // for each polygon
  for( INDEX iPoly=0; iPoly<edm_md.md_MipInfos[iCurrentMip].mmpi_PolygonsCt; iPoly++)
  {
    struct ModelPolygon *pPoly = &edm_md.md_MipInfos[iCurrentMip].mmpi_Polygons[iPoly];
    if( pPoly->mp_Surface == iCurrentSurface)
    { // readout poly vertices
      f3dTr0(1) = (FLOAT)pPoly->mp_PolygonVertices[0].mpv_ptvTextureVertex->mtv_UV(1);
      f3dTr0(2) = (FLOAT)pPoly->mp_PolygonVertices[0].mpv_ptvTextureVertex->mtv_UV(2);
      f3dTr0(3) = 0.0f;
      f3dTr1(1) = (FLOAT)pPoly->mp_PolygonVertices[1].mpv_ptvTextureVertex->mtv_UV(1);
      f3dTr1(2) = (FLOAT)pPoly->mp_PolygonVertices[1].mpv_ptvTextureVertex->mtv_UV(2);
      f3dTr1(3) = 0.0f;
      f3dTr2(1) = (FLOAT)pPoly->mp_PolygonVertices[2].mpv_ptvTextureVertex->mtv_UV(1);
      f3dTr2(2) = (FLOAT)pPoly->mp_PolygonVertices[2].mpv_ptvTextureVertex->mtv_UV(2);
      f3dTr2(3) = 0.0f;

      // determine line visibility
      FLOAT3D f3dNormal = (f3dTr2-f3dTr1)*(f3dTr0-f3dTr1);
      COLOR clrWire;
      ULONG ulLineType;
      if( f3dNormal(3) < 0) {
        clrWire = clrVisible;
        ulLineType = _FULL_;
      } else {
        clrWire = clrInvisible;
        ulLineType = _POINT_;
      }
      // draw lines
      PIX pixX0, pixY0, pixX1, pixY1;
      for( INDEX iVtx=0; iVtx<pPoly->mp_PolygonVertices.Count()-1; iVtx++) {
        pVtx0 = pPoly->mp_PolygonVertices[iVtx+0].mpv_ptvTextureVertex;
        pVtx1 = pPoly->mp_PolygonVertices[iVtx+1].mpv_ptvTextureVertex;
        pixX0 = (PIX)(pVtx0->mtv_UV(1) * fMagnifyFactor) - offx;
        pixY0 = (PIX)(pVtx0->mtv_UV(2) * fMagnifyFactor) - offy;
        pixX1 = (PIX)(pVtx1->mtv_UV(1) * fMagnifyFactor) - offx;
        pixY1 = (PIX)(pVtx1->mtv_UV(2) * fMagnifyFactor) - offy;
        pDP->DrawLine( pixX0, pixY0, pixX1, pixY1, clrWire|CT_OPAQUE, ulLineType);
      }
      // draw last line
      pVtx0 = pPoly->mp_PolygonVertices[0].mpv_ptvTextureVertex;
      pixX0 = (PIX)(pVtx0->mtv_UV(1) * fMagnifyFactor) - offx;
      pixY0 = (PIX)(pVtx0->mtv_UV(2) * fMagnifyFactor) - offy;
      pDP->DrawLine( pixX0, pixY0, pixX1, pixY1, clrWire|CT_OPAQUE, ulLineType);
    }
  }
}


/*
 * Flat fills given surface
 */
void CEditModel::DrawFilledSurface( CDrawPort *pDP, INDEX iCurrentMip, INDEX iCurrentSurface,
                                    FLOAT fMagnifyFactor, PIX offx, PIX offy,
                                    COLOR clrVisible, COLOR clrInvisible)
{
  FLOAT3D f3dTr0, f3dTr1, f3dTr2;
  struct ModelTextureVertex *pVtx0, *pVtx1, *pVtx2;

  // for each polygon
  for( INDEX iPoly=0; iPoly<edm_md.md_MipInfos[iCurrentMip].mmpi_PolygonsCt; iPoly++)
  {
    struct ModelPolygon *pPoly = &edm_md.md_MipInfos[iCurrentMip].mmpi_Polygons[iPoly];
    if( pPoly->mp_Surface == iCurrentSurface)
    { // readout poly vertices
      f3dTr0(1) = (FLOAT)pPoly->mp_PolygonVertices[0].mpv_ptvTextureVertex->mtv_UV(1);
      f3dTr0(2) = (FLOAT)pPoly->mp_PolygonVertices[0].mpv_ptvTextureVertex->mtv_UV(2);
      f3dTr0(3) = 0.0f;
      f3dTr1(1) = (FLOAT)pPoly->mp_PolygonVertices[1].mpv_ptvTextureVertex->mtv_UV(1);
      f3dTr1(2) = (FLOAT)pPoly->mp_PolygonVertices[1].mpv_ptvTextureVertex->mtv_UV(2);
      f3dTr1(3) = 0.0f;
      f3dTr2(1) = (FLOAT)pPoly->mp_PolygonVertices[2].mpv_ptvTextureVertex->mtv_UV(1);
      f3dTr2(2) = (FLOAT)pPoly->mp_PolygonVertices[2].mpv_ptvTextureVertex->mtv_UV(2);
      f3dTr2(3) = 0.0f;

      // determine poly visibility
      COLOR clrFill;
      FLOAT3D f3dNormal = (f3dTr2-f3dTr1)*(f3dTr0-f3dTr1);
      if( f3dNormal(3) < 0) clrFill = clrVisible|0xFF;
      else clrFill = clrInvisible|0xFF;

      // draw traingle(s) fan
      pDP->InitTexture( NULL);
      pVtx0 = pPoly->mp_PolygonVertices[0].mpv_ptvTextureVertex;
      PIX pixX0 = (PIX)(pVtx0->mtv_UV(1) * fMagnifyFactor) - offx;
      PIX pixY0 = (PIX)(pVtx0->mtv_UV(2) * fMagnifyFactor) - offy;
      for( INDEX iVtx=1; iVtx<pPoly->mp_PolygonVertices.Count()-1; iVtx++) {
        pVtx1 = pPoly->mp_PolygonVertices[iVtx+0].mpv_ptvTextureVertex;
        pVtx2 = pPoly->mp_PolygonVertices[iVtx+1].mpv_ptvTextureVertex;
        PIX pixX1 = (PIX)(pVtx1->mtv_UV(1) * fMagnifyFactor) - offx;
        PIX pixY1 = (PIX)(pVtx1->mtv_UV(2) * fMagnifyFactor) - offy;
        PIX pixX2 = (PIX)(pVtx2->mtv_UV(1) * fMagnifyFactor) - offx;
        PIX pixY2 = (PIX)(pVtx2->mtv_UV(2) * fMagnifyFactor) - offy;
        pDP->AddTriangle( pixX0,pixY0, pixX1,pixY1, pixX2,pixY2, clrFill);
      }
      // to buffer with it
      pDP->FlushRenderingQueue();
    }
  }
}


/*
 * Prints surface numbers
 */
void CEditModel::PrintSurfaceNumbers( CDrawPort *pDP, CFontData *pFont,
     INDEX iCurrentMip, FLOAT fMagnifyFactor, PIX offx, PIX offy, COLOR clrInk)
{
  char achrLine[ 256];

   // clear Z-buffer
  pDP->FillZBuffer( ZBUF_BACK);

  // get mip model ptr
  struct ModelMipInfo *pMMI = &edm_md.md_MipInfos[ iCurrentMip];


  // for all surfaces
  for( INDEX iSurf=0;iSurf<pMMI->mmpi_MappingSurfaces.Count(); iSurf++)
  {
    MappingSurface *pms= &pMMI->mmpi_MappingSurfaces[iSurf];
    MEXaabbox2D boxSurface;
    // for each texture vertex in surface
    for(INDEX iSurfaceTextureVertex=0; iSurfaceTextureVertex<pms->ms_aiTextureVertices.Count(); iSurfaceTextureVertex++)
    {
      INDEX iGlobalTextureVertex = pms->ms_aiTextureVertices[iSurfaceTextureVertex];
      ModelTextureVertex *pmtv = &pMMI->mmpi_TextureVertices[iGlobalTextureVertex];
      boxSurface |= pmtv->mtv_UV;
    }
   
    MEX2D mexCenter = boxSurface.Center();
    PIX2D pixCenter = PIX2D(mexCenter(1)*fMagnifyFactor-offx, mexCenter(2)*fMagnifyFactor-offy);

    // print active surface's number into print line
    sprintf( achrLine, "%d", iSurf);

    // set font
    pDP->SetFont( pFont);
    // print line
    pDP->PutText( achrLine, pixCenter(1)-strlen(achrLine)*4, pixCenter(2)-6);
  }
}

/*
 * Exports surface names and numbers under given file name
 */
void CEditModel::ExportSurfaceNumbersAndNames( CTFileName fnFile)
{
  CTString strExport;
  // get mip model ptr
  struct ModelMipInfo *pMMI = &edm_md.md_MipInfos[ 0];

  // for all surfaces
  for( INDEX iSurf=0; iSurf<pMMI->mmpi_MappingSurfaces.Count(); iSurf++)
  {
    MappingSurface *pms= &pMMI->mmpi_MappingSurfaces[iSurf];
    CTString strExportLine;
    strExportLine.PrintF( "%d) %s\n", iSurf, pms->ms_Name);
    strExport+=strExportLine;
  }

  try
  {
    strExport.Save_t( fnFile);
  }
  catch(char *strError)
  {
    // report error
    WarningMessage( strError);
  }
}

/*
 * Retrieves given surface's name
 */
const char *CEditModel::GetSurfaceName(INDEX iCurrentMip, INDEX iCurrentSurface)
{
  struct MappingSurface *pSurface;
  pSurface = &edm_md.md_MipInfos[ iCurrentMip].mmpi_MappingSurfaces[ iCurrentSurface];
  return( pSurface->ms_Name);
}
//--------------------------------------------------------------------------------------------
/*
 * Sets first empty position in existing patches mask
 */
BOOL CEditModel::GetFirstEmptyPatchIndex( INDEX &iMaskBit)
{
  iMaskBit = 0;
  for( INDEX iPatch=0; iPatch<MAX_TEXTUREPATCHES; iPatch++)
  {
    CTextureData *pTD = (CTextureData *) edm_md.md_mpPatches[ iPatch].mp_toTexture.GetData();
    if( pTD == NULL)
    {
      iMaskBit = iPatch;
      return TRUE;
    }
  }
  return FALSE;
}
//--------------------------------------------------------------------------------------------
/*
 * Sets first occupied position in existing patches mask
 */
BOOL CEditModel::GetFirstValidPatchIndex( INDEX &iMaskBit)
{
  iMaskBit = 0;
  for( INDEX iPatch=0; iPatch<MAX_TEXTUREPATCHES; iPatch++)
  {
    CTextureData *pTD = (CTextureData *) edm_md.md_mpPatches[ iPatch].mp_toTexture.GetData();
    if( pTD != NULL)
    {
      iMaskBit = iPatch;
      return TRUE;
    }
  }
  return FALSE;
}
//--------------------------------------------------------------------------------------------
/*
 * Sets previous valid patch position in existing patches mask
 */
void CEditModel::GetPreviousValidPatchIndex( INDEX &iMaskBit)
{
  ASSERT( (iMaskBit>=0) && (iMaskBit<MAX_TEXTUREPATCHES) );
  for( INDEX iPatch=iMaskBit+MAX_TEXTUREPATCHES-1; iPatch>iMaskBit; iPatch--)
  {
    INDEX iCurrentPatch = iPatch%32;
    CTString strPatchName = edm_md.md_mpPatches[ iCurrentPatch].mp_strName;
    if( strPatchName != "")
    {
      iMaskBit = iCurrentPatch;
      return;
    }
  }
}
//--------------------------------------------------------------------------------------------
/*
 * Sets next valid patch position in existing patches mask
 */
void CEditModel::GetNextValidPatchIndex( INDEX &iMaskBit)
{
  ASSERT( (iMaskBit>=0) && (iMaskBit<MAX_TEXTUREPATCHES) );
  for( INDEX iPatch=iMaskBit+1; iPatch<iMaskBit+MAX_TEXTUREPATCHES; iPatch++)
  {
    INDEX iCurrentPatch = iPatch%32;
    CTString strPatchName = edm_md.md_mpPatches[ iCurrentPatch].mp_strName;
    if( strPatchName != "")
    {
      iMaskBit = iCurrentPatch;
      return;
    }
  }
}
//--------------------------------------------------------------------------------------------
/*
 * Moves patch relatively for given coordinates
 */
void CEditModel::MovePatchRelative( INDEX iMaskBit, MEX2D mexOffset)
{
  CTFileName fnPatch = edm_md.md_mpPatches[ iMaskBit].mp_toTexture.GetName();
  if( fnPatch == "") return;
  edm_md.md_mpPatches[ iMaskBit].mp_mexPosition += mexOffset;
  CalculatePatchesPerPolygon();
}
//--------------------------------------------------------------------------------------------
/*
 * Sets patch stretch
 */
void CEditModel::SetPatchStretch( INDEX iMaskBit, FLOAT fNewStretch)
{
  CTFileName fnPatch = edm_md.md_mpPatches[ iMaskBit].mp_toTexture.GetName();
  if( fnPatch == "") return;
  edm_md.md_mpPatches[ iMaskBit].mp_fStretch = fNewStretch;
  CalculatePatchesPerPolygon();
}
//--------------------------------------------------------------------------------------------
/*
 * Searches for first available empty patch position index and adds patch
 */
BOOL CEditModel::EditAddPatch( CTFileName fnPatchName, MEX2D mexPos, INDEX &iMaskBit)
{
  if( !GetFirstEmptyPatchIndex( iMaskBit))
    return FALSE;

  try
  {
    edm_md.md_mpPatches[ iMaskBit].mp_toTexture.SetData_t( fnPatchName);
  }
  catch (char *strError)
  {
    (void)strError;
    return FALSE;
  }
  edm_md.md_mpPatches[ iMaskBit].mp_mexPosition = mexPos;
  edm_md.md_mpPatches[ iMaskBit].mp_fStretch = 1.0f;
  edm_md.md_mpPatches[ iMaskBit].mp_strName.PrintF( "Patch%02d", iMaskBit);
  CalculatePatchesPerPolygon();
  return TRUE;
}
//--------------------------------------------------------------------------------------------
/*
 * Removes patch with given index from existing mask and erases its file name
 */
void CEditModel::EditRemovePatch( INDEX iMaskBit)
{
  edm_md.md_mpPatches[ iMaskBit].mp_toTexture.SetData(NULL);
  CalculatePatchesPerPolygon();
}

void CEditModel::EditRemoveAllPatches(void)
{
  for( INDEX iPatch=0; iPatch<MAX_TEXTUREPATCHES; iPatch++)
  {
    edm_md.md_mpPatches[ iPatch].mp_toTexture.SetData(NULL);
  }
  CalculatePatchesPerPolygon();
}

INDEX CEditModel::CountPatches(void)
{
  INDEX iResult = 0;
  for(INDEX iPatch=0; iPatch<MAX_TEXTUREPATCHES; iPatch++)
  {
    if( edm_md.md_mpPatches[ iPatch].mp_toTexture.GetName() != "")
    {
      iResult++;
    }
  }
  return iResult;
}

ULONG CEditModel::GetExistingPatchesMask(void)
{
  ULONG ulResult = 0;
  for(INDEX iPatch=0; iPatch<MAX_TEXTUREPATCHES; iPatch++)
  {
    if( edm_md.md_mpPatches[ iPatch].mp_toTexture.GetName() != "")
    {
      ulResult |= 1UL << iPatch;
    }
  }
  return ulResult;
}
//--------------------------------------------------------------------------------------------
void CEditModel::CalculatePatchesPerPolygon(void)
{
  // count existing patches
  INDEX ctPatches = CountPatches();

  // for each mip model
  for( INDEX iMip=0; iMip<edm_md.md_MipCt; iMip++)
  {
    ModelMipInfo *pMMI = &edm_md.md_MipInfos[ iMip];
    // clear previously existing array
    pMMI->mmpi_aPolygonsPerPatch.Clear();
    // if patches are visible in this mip model
    if( (pMMI->mmpi_ulFlags & MM_PATCHES_VISIBLE) && (ctPatches != 0) )
    {
      // add description member for each patch
      pMMI->mmpi_aPolygonsPerPatch.New( ctPatches);
      INDEX iExistingPatch = 0;
      // for each patch
      for(INDEX iPatch=0; iPatch<MAX_TEXTUREPATCHES; iPatch++)
      {
        // if patch exists
        if( edm_md.md_mpPatches[ iPatch].mp_toTexture.GetName() != "")
        {
          // allocate temporary array of indices for each polygon in mip model
          CStaticArray<INDEX> aiPolygons;
          aiPolygons.New( pMMI->mmpi_PolygonsCt);
          // clear counter of occupied polygons
          INDEX ctOccupiedPolygons = 0;
          // get patch occupying box
          CTextureData *pTD = (CTextureData *) edm_md.md_mpPatches[ iPatch].mp_toTexture.GetData();
          ASSERT( pTD != NULL);
          MEX2D mex2dPosition = edm_md.md_mpPatches[ iPatch].mp_mexPosition;
          FLOAT fStretch = edm_md.md_mpPatches[ iPatch].mp_fStretch;
          MEXaabbox2D boxPatch = MEXaabbox2D(
                  mex2dPosition, MEX2D( mex2dPosition(1)+pTD->GetWidth()*fStretch,
                                 mex2dPosition(2)+pTD->GetHeight()*fStretch) );
          // for each polygon
          for(INDEX iPolygon=0; iPolygon<pMMI->mmpi_PolygonsCt; iPolygon++)
          {
            ModelPolygon *pMP = &pMMI->mmpi_Polygons[iPolygon];
            // for all vertices in polygon
            MEXaabbox2D boxMapping;
            for( INDEX iVertex=0; iVertex<pMP->mp_PolygonVertices.Count(); iVertex++)
            {
              ModelTextureVertex *pMTV = pMP->mp_PolygonVertices[iVertex].mpv_ptvTextureVertex;
              // calculate bounding box of mapping coordinates
              boxMapping |= MEXaabbox2D(pMTV->mtv_UV);
            }
            // if bounding box of polygon's mapping coordinates touches patch
            if( boxPatch.HasContactWith( boxMapping))
            {
              // add polygon index to list of occupied polygons
              aiPolygons[ ctOccupiedPolygons] = iPolygon;
              ctOccupiedPolygons++;
            }
          }
          if( ctOccupiedPolygons != 0)
          {
            // copy temporary array of polygon indices to mip model's array of polygon indices
            pMMI->mmpi_aPolygonsPerPatch[ iExistingPatch].ppp_iPolygons.New( ctOccupiedPolygons);
            for( INDEX iOccupied=0; iOccupied<ctOccupiedPolygons; iOccupied++)
            {
              pMMI->mmpi_aPolygonsPerPatch[ iExistingPatch].ppp_iPolygons[iOccupied] =
                aiPolygons[ iOccupied];
            }
          }
          // count existing patches
          iExistingPatch++;
        }
      }
    }
  }
}
//--------------------------------------------------------------------------------------------
/*
 * Writes settings of given mip model into file
 */
void CEditModel::WriteMipSettings_t( CTStream *ostrFile, INDEX iMip)
{
  ASSERT( iMip < edm_md.md_MipCt);

  // write indetification of one mip's mapping info
  ostrFile->WriteID_t( CChunkID( "MIPS"));

  // get count
  INDEX iSurfacesCt = edm_md.md_MipInfos[ iMip].mmpi_MappingSurfaces.Count();
  // write count
  (*ostrFile) << iSurfacesCt;
  // for all surfaces
  FOREACHINSTATICARRAY(edm_md.md_MipInfos[ iMip].mmpi_MappingSurfaces, MappingSurface, itSurface)
  {
    // write setings for current surface
    itSurface->WriteSettings_t( ostrFile);
  }
}
//--------------------------------------------------------------------------------------------
/*
 * Reads settigns of given mip model from file
 */
void CEditModel::ReadMipSettings_t(CTStream *istrFile, INDEX iMip)
{
  MappingSurface msTmp;

  ASSERT( iMip < edm_md.md_MipCt);

  // check chunk
  istrFile->ExpectID_t( CChunkID( "MIPS"));
  // get count
  INDEX iSurfacesCt;
  *istrFile >> iSurfacesCt;

  // for all saved surfaces
  for( INDEX iSurface=0; iSurface<iSurfacesCt; iSurface++)
  {
    // read mapping surface settings
    msTmp.ReadSettings_t( istrFile);

    // for all surfaces in given mip
    for( INDEX i=0; i<edm_md.md_MipInfos[ iMip].mmpi_MappingSurfaces.Count(); i++)
    {
      MappingSurface &ms = edm_md.md_MipInfos[ iMip].mmpi_MappingSurfaces[ i];
      // are these surfaces the same?
      if( ms == msTmp)
      {
        // try to set new position and angles
        ms.ms_sstShadingType = msTmp.ms_sstShadingType;
        ms.ms_sttTranslucencyType = msTmp.ms_sttTranslucencyType;
        ms.ms_ulRenderingFlags = msTmp.ms_ulRenderingFlags;
        ms.ms_colDiffuse = msTmp.ms_colDiffuse;
        ms.ms_colReflections = msTmp.ms_colReflections;
        ms.ms_colSpecular = msTmp.ms_colSpecular;
        ms.ms_colBump = msTmp.ms_colBump;
        ms.ms_ulOnColor = msTmp.ms_ulOnColor;
        ms.ms_ulOffColor = msTmp.ms_ulOffColor;
      }
    }
  }
}
//--------------------------------------------------------------------------------------------
/*
 * Saves mapping data for whole model (iMip = -1) or for just one mip model
 */
void CEditModel::SaveMapping_t( CTFileName fnFileName, INDEX iMip /*=-1*/)
{
  CTFileStream strmMappingFile;

  // create file
  strmMappingFile.Create_t( fnFileName, CTStream::CM_BINARY);
  // write file ID
  strmMappingFile.WriteID_t( CChunkID( "MPNG"));
  // write version
  strmMappingFile.WriteID_t( CChunkID(MAPPING_VERSION));

  // set as we have only one mip
  INDEX iStartCt = iMip;
  INDEX iMaxCt = iMip+1;

  // if iMip is -1 means that we want all mips in model
  if( iMip == -1)
  {
    iStartCt = 0;
    iMaxCt = edm_md.md_MipCt;
  }

  // for wanted mip models
  for( INDEX iMipCt=iStartCt; iMipCt<iMaxCt; iMipCt++)
  {
    // write settings for current mip
    WriteMipSettings_t( &strmMappingFile, iMipCt);
  }

  // save attached sounds
  strmMappingFile<<edm_aasAttachedSounds.Count();
  for( INDEX iSound=0; iSound<edm_aasAttachedSounds.Count(); iSound++)
  {
    edm_aasAttachedSounds[iSound].Write_t( &strmMappingFile);
  }

  // save attached models
  INDEX ctAttachmentPositions = edm_aamAttachedModels.Count();
  ASSERT( edm_md.md_aampAttachedPosition.Count() == ctAttachmentPositions);
  strmMappingFile<<ctAttachmentPositions;
  FOREACHINDYNAMICARRAY(edm_aamAttachedModels, CAttachedModel, itam)
  {
    itam->Write_t( &strmMappingFile);
  }
  FOREACHINDYNAMICARRAY(edm_md.md_aampAttachedPosition, CAttachedModelPosition, itamp)
  {
    itamp->Write_t( &strmMappingFile);
  }

  // save collision boxes
  INDEX ctCollisionBoxes = edm_md.md_acbCollisionBox.Count();
  ASSERT( ctCollisionBoxes>0);
  if(ctCollisionBoxes == 0)
  {
    WarningMessage( "Trying to save 0 collision boxes into mapping file.");
  }
  strmMappingFile<<ctCollisionBoxes;
  FOREACHINDYNAMICARRAY(edm_md.md_acbCollisionBox, CModelCollisionBox, itcb)
  {
    itcb->Write_t( &strmMappingFile);
  }

  // save patches
  for( INDEX iPatch=0; iPatch<MAX_TEXTUREPATCHES; iPatch++)
  {
    edm_md.md_mpPatches[ iPatch].Write_t( &strmMappingFile);
  }
}
//--------------------------------------------------------------------------------------------
/*
 * Loads mapping data for whole model (iMip = -1) or just for one mip model
 */
void CEditModel::LoadMapping_t( CTFileName fnFileName, INDEX iMip /*=-1*/)
{
  CTFileStream strmMappingFile;

  BOOL bReadPolygonsPerSurface = FALSE;
  BOOL bReadSoundsAndAttachments = FALSE;
  BOOL bReadCollision = FALSE;
  BOOL bReadPatches = FALSE;
  BOOL bReadSurfaceColors = FALSE;
  // open binary file
  strmMappingFile.Open_t( fnFileName);
  // recognize file ID
  strmMappingFile.ExpectID_t( CChunkID( "MPNG"));
  // get version of mapping file
  CChunkID cidVersion = strmMappingFile.GetID_t();
  // act acording to version of mapping file
  if( cidVersion == CChunkID(MAPPING_VERSION_WITHOUT_POLYGONS_PER_SURFACE) )
  {
  }
  else if( cidVersion == CChunkID( MAPPING_VERSION_WITHOUT_SOUNDS_AND_ATTACHMENTS))
  {
    bReadPolygonsPerSurface = TRUE;
  }
  else if( cidVersion == CChunkID( MAPPING_VERSION_WITHOUT_COLLISION))
  {
    bReadPolygonsPerSurface = TRUE;
    bReadSoundsAndAttachments = TRUE;
  }
  else if( cidVersion == CChunkID( MAPPING_VERSION_WITHOUT_PATCHES))
  {
    bReadPolygonsPerSurface = TRUE;
    bReadSoundsAndAttachments = TRUE;
    bReadCollision = TRUE;
  }
  else if( cidVersion == CChunkID( MAPPING_WITHOUT_SURFACE_COLORS))
  {
    bReadPolygonsPerSurface = TRUE;
    bReadSoundsAndAttachments = TRUE;
    bReadCollision = TRUE;
    bReadPatches = TRUE;
  }
  else if( cidVersion == CChunkID( MAPPING_VERSION))
  {
    bReadPolygonsPerSurface = TRUE;
    bReadSoundsAndAttachments = TRUE;
    bReadCollision = TRUE;
    bReadPatches = TRUE;
    bReadSurfaceColors = TRUE;
  }
  else
  {
    throw( "Invalid version of mapping file.");
  }
  // set as we have only one mip
  INDEX iStartCt = iMip;
  INDEX iMaxCt = iMip+1;

  // if iMip is -1 means that we want all mips in model
  if( iMip == -1)
  {
    iStartCt = 0;
    iMaxCt = edm_md.md_MipCt;
  }

  // for wanted mip models
  for( INDEX iMipCt=iStartCt; iMipCt<iMaxCt; iMipCt++)
  {
    if( strmMappingFile.PeekID_t()==CChunkID("MIPS"))
    {
      // read mapping for current mip
      ReadMipSettings_t( &strmMappingFile, iMipCt);
    }
  }

  // skip data for mip models that were saved but haven't been
  // readed in previous loop
  while( strmMappingFile.PeekID_t()==CChunkID("MIPS"))
  {
    MappingSurface msDummy;
    strmMappingFile.ExpectID_t( CChunkID( "MIPS"));
    // for all saved surfaces
    INDEX iSurfacesCt;
    strmMappingFile >> iSurfacesCt;
    for( INDEX iSurface=0; iSurface<iSurfacesCt; iSurface++)
    {
      // skip mapping surface
      msDummy.ReadSettings_t( &strmMappingFile);
    }
  }

  if( bReadSoundsAndAttachments)
  {
    // load attached sounds
    INDEX ctSounds;
    strmMappingFile>>ctSounds;
    ASSERT(ctSounds > 0);
    edm_aasAttachedSounds.Clear();
    edm_aasAttachedSounds.New( ctSounds);
    for( INDEX iSound=0; iSound<edm_aasAttachedSounds.Count(); iSound++)
    {
      edm_aasAttachedSounds[iSound].Read_t( &strmMappingFile);
    }
    // if number of animations does not match number of sounds saved in map file, reset sounds
    if(ctSounds != edm_md.GetAnimsCt())
    {
      edm_aasAttachedSounds.Clear();
      edm_aasAttachedSounds.New( edm_md.GetAnimsCt());
    }

    // load attached models
    INDEX ctAttachmentPositions;
    strmMappingFile>>ctAttachmentPositions;
    edm_aamAttachedModels.Clear();
    edm_md.md_aampAttachedPosition.Clear();
    if( ctAttachmentPositions != 0)
    {
      edm_aamAttachedModels.New(ctAttachmentPositions);
      edm_md.md_aampAttachedPosition.New(ctAttachmentPositions);
      FOREACHINDYNAMICARRAY(edm_aamAttachedModels, CAttachedModel, itam)
      {
        try
        {
          itam->Read_t( &strmMappingFile);
        }
        catch( char *strError)
        {
          (void) strError;
          edm_aamAttachedModels.Clear();
          edm_md.md_aampAttachedPosition.Clear();
          ThrowF_t( "Error ocured while reading attahment model, maybe model does"
                    " not exist.");
        }
      }
      FOREACHINDYNAMICARRAY(edm_md.md_aampAttachedPosition, CAttachedModelPosition, itamp)
      {
        itamp->Read_t( &strmMappingFile);
      }
    }
  }

  if( bReadCollision)
  {
    // read collision boxes
    edm_md.md_acbCollisionBox.Clear();
    INDEX ctCollisionBoxes;
    strmMappingFile>>ctCollisionBoxes;
    ASSERT(ctCollisionBoxes>0);
    if( ctCollisionBoxes>0)
    {
      edm_md.md_acbCollisionBox.New( ctCollisionBoxes);
      FOREACHINDYNAMICARRAY(edm_md.md_acbCollisionBox, CModelCollisionBox, itcb)
      {
        itcb->Read_t( &strmMappingFile);
        itcb->ReadName_t( &strmMappingFile);
      }
    }
    else
    {
      edm_md.md_acbCollisionBox.New( 1);
      throw( "Trying to load 0 collision boxes from mapping file.");
    }
  }

  if( bReadPatches)
  {
    EditRemoveAllPatches();
    for( INDEX iPatch=0; iPatch<MAX_TEXTUREPATCHES; iPatch++)
    {
      edm_md.md_mpPatches[ iPatch].Read_t( &strmMappingFile);
    }
    CalculatePatchesPerPolygon();
  }
}

void CEditModel::AddCollisionBox(void)
{
  // add one collision box
  edm_md.md_acbCollisionBox.New();
  // select newly added collision box
  edm_iActiveCollisionBox = edm_md.md_acbCollisionBox.Count()-1;
}

void CEditModel::DeleteCurrentCollisionBox(void)
{
  INDEX ctCollisionBoxes = edm_md.md_acbCollisionBox.Count();
  // if we have more than 1 collision box
  if( ctCollisionBoxes != 1)
  {
    edm_md.md_acbCollisionBox.Lock();
    edm_md.md_acbCollisionBox.Delete( &edm_md.md_acbCollisionBox[ edm_iActiveCollisionBox]);
    edm_md.md_acbCollisionBox.Unlock();
    // if this was last collision box
    if( edm_iActiveCollisionBox == (ctCollisionBoxes-1) )
    {
      // select last collision box
      edm_iActiveCollisionBox = ctCollisionBoxes-2;
    }
  }
}

void CEditModel::ActivatePreviousCollisionBox(void)
{
  // get count of collision boxes
  INDEX ctCollisionBoxes = edm_md.md_acbCollisionBox.Count();
  if( edm_iActiveCollisionBox != 0)
  {
    edm_iActiveCollisionBox -= 1;
  }
}

void CEditModel::ActivateNextCollisionBox(void)
{
  // get count of collision boxes
  INDEX ctCollisionBoxes = edm_md.md_acbCollisionBox.Count();
  if( edm_iActiveCollisionBox != (ctCollisionBoxes-1) )
  {
    edm_iActiveCollisionBox += 1;
  }
}

void CEditModel::SetCollisionBox(FLOAT3D vMin, FLOAT3D vMax)
{
  edm_md.md_acbCollisionBox.Lock();
  edm_md.md_acbCollisionBox[ edm_iActiveCollisionBox].mcb_vCollisionBoxMin = vMin;
  edm_md.md_acbCollisionBox[ edm_iActiveCollisionBox].mcb_vCollisionBoxMax = vMax;
  edm_md.md_acbCollisionBox.Unlock();
  CorrectCollisionBoxSize();
}

CTString CEditModel::GetCollisionBoxName(INDEX iCollisionBox)
{
  // get count of collision boxes
  INDEX ctCollisionBoxes = edm_md.md_acbCollisionBox.Count();
  ASSERT( iCollisionBox < ctCollisionBoxes);
  if( iCollisionBox >= ctCollisionBoxes)
  {
    iCollisionBox = ctCollisionBoxes-1;
  }
  CTString strCollisionBoxName;
  edm_md.md_acbCollisionBox.Lock();
  strCollisionBoxName = edm_md.md_acbCollisionBox[ iCollisionBox].mcb_strName;
  edm_md.md_acbCollisionBox.Unlock();
  return strCollisionBoxName;
}

CTString CEditModel::GetCollisionBoxName(void)
{
  CTString strCollisionBoxName;
  edm_md.md_acbCollisionBox.Lock();
  strCollisionBoxName = edm_md.md_acbCollisionBox[ edm_iActiveCollisionBox].mcb_strName;
  edm_md.md_acbCollisionBox.Unlock();
  return strCollisionBoxName;
}

void CEditModel::SetCollisionBoxName(CTString strNewName)
{
  edm_md.md_acbCollisionBox.Lock();
  edm_md.md_acbCollisionBox[ edm_iActiveCollisionBox].mcb_strName = strNewName;
  edm_md.md_acbCollisionBox.Unlock();
}

void CEditModel::CorrectCollisionBoxSize(void)
{
  // no correction needed if colliding as cube
  if( edm_md.md_bCollideAsCube) return;
  edm_md.md_acbCollisionBox.Lock();
  // get equality radio initial value
  INDEX iEqualityType = GetCollisionBoxDimensionEquality();
  // get min and max vectors of currently active collision box
  FLOAT3D vMin = edm_md.md_acbCollisionBox[ edm_iActiveCollisionBox].mcb_vCollisionBoxMin;
  FLOAT3D vMax = edm_md.md_acbCollisionBox[ edm_iActiveCollisionBox].mcb_vCollisionBoxMax;
  FLOAT3D vOldCenter;

  vOldCenter(1) = (vMax(1)+vMin(1))/2.0f;
  vOldCenter(3) = (vMax(3)+vMin(3))/2.0f;

  // calculate vector of collision box diagonale
  FLOAT3D vCorrectedDiagonale = vMax-vMin;
  // apply minimal collision box conditions
  if( vCorrectedDiagonale(1) < 0.1f) vCorrectedDiagonale(1) = 0.01f;
  if( vCorrectedDiagonale(2) < 0.1f) vCorrectedDiagonale(2) = 0.01f;
  if( vCorrectedDiagonale(3) < 0.1f) vCorrectedDiagonale(3) = 0.01f;
  // according to equality type flag (which dimensions are same)
  switch( iEqualityType)
  {
    case HEIGHT_EQ_WIDTH:
    {
      // don't allow that unlocked dimension is smaller than locked ones
      if( vCorrectedDiagonale(3) < vCorrectedDiagonale(1) )
      {
        vCorrectedDiagonale(3) = vCorrectedDiagonale(1);
      }
      // height = width
      vCorrectedDiagonale(2) = vCorrectedDiagonale(1);
      break;
    }
    case LENGTH_EQ_WIDTH:
    {
      // don't allow that unlocked dimension is smaller than locked ones
      if( vCorrectedDiagonale(2) < vCorrectedDiagonale(1) )
      {
        vCorrectedDiagonale(2) = vCorrectedDiagonale(1);
      }
      // lenght = width
      vCorrectedDiagonale(3) = vCorrectedDiagonale(1);
      break;
    }
    case LENGTH_EQ_HEIGHT:
    {
      // don't allow that unlocked dimension is smaller than locked ones
      if( vCorrectedDiagonale(1) < vCorrectedDiagonale(2) )
      {
        vCorrectedDiagonale(1) = vCorrectedDiagonale(2);
      }
      // lenght = height
      vCorrectedDiagonale(3) = vCorrectedDiagonale(2);
      break;
    }
    default:
    {
      ASSERTALWAYS( "Invalid collision box dimension equality value found.");
    }
  }
  // set new, corrected max vector
  FLOAT3D vNewMin, vNewMax;
  vNewMin(1) = vOldCenter(1)-vCorrectedDiagonale(1)/2.0f;
  vNewMin(2) = vMin(2);
  vNewMin(3) = vOldCenter(3)-vCorrectedDiagonale(3)/2.0f;

  vNewMax(1) = vOldCenter(1)+vCorrectedDiagonale(1)/2.0f;
  vNewMax(2) = vMin(2)+vCorrectedDiagonale(2);
  vNewMax(3) = vOldCenter(3)+vCorrectedDiagonale(3)/2.0f;

  edm_md.md_acbCollisionBox[ edm_iActiveCollisionBox].mcb_vCollisionBoxMin = vNewMin;
  edm_md.md_acbCollisionBox[ edm_iActiveCollisionBox].mcb_vCollisionBoxMax = vNewMax;
  edm_md.md_acbCollisionBox.Unlock();
}
//---------------------------------------------------------------------------------------------
// collision box handling functions
FLOAT3D &CEditModel::GetCollisionBoxMin(void)
{
  edm_md.md_acbCollisionBox.Lock();
  FLOAT3D &vMin = edm_md.md_acbCollisionBox[edm_iActiveCollisionBox].mcb_vCollisionBoxMin;
  edm_md.md_acbCollisionBox.Unlock();
  return vMin;
};
FLOAT3D &CEditModel::GetCollisionBoxMax(void)
{
  edm_md.md_acbCollisionBox.Lock();
  FLOAT3D &vMax = edm_md.md_acbCollisionBox[edm_iActiveCollisionBox].mcb_vCollisionBoxMax;
  edm_md.md_acbCollisionBox.Unlock();
  return vMax;
};

// returns HEIGHT_EQ_WIDTH, LENGHT_EQ_WIDTH or LENGHT_EQ_HEIGHT
INDEX CEditModel::GetCollisionBoxDimensionEquality()
{
  return edm_md.GetCollisionBoxDimensionEquality(edm_iActiveCollisionBox);
};
// set new collision box equality value
void CEditModel::SetCollisionBoxDimensionEquality( INDEX iNewDimEqType)
{
  edm_md.md_acbCollisionBox.Lock();
  edm_md.md_acbCollisionBox[edm_iActiveCollisionBox].mcb_iCollisionBoxDimensionEquality =
    iNewDimEqType;
  edm_md.md_acbCollisionBox.Unlock();
  CorrectCollisionBoxSize();
};
