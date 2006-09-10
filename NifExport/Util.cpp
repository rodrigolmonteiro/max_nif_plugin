#include "pch.h"
#include <obj/NiLight.h>
#include <obj/NiAmbientLight.h>
#include <obj/NiPointLight.h>
#include <obj/NiDirectionalLight.h>
#include <obj/NiSpotLight.h>

bool Exporter::TMNegParity(const Matrix3 &m)
{
	return (DotProd(CrossProd(m.GetRow(0),m.GetRow(1)),m.GetRow(2))<0.0)?true:false;
}

Point3 Exporter::getVertexNormal(Mesh* mesh, int faceNo, RVertex* rv)
{
	Face* f = &mesh->faces[faceNo];
	DWORD smGroup = f->smGroup;
	int numNormals;
	Point3 vertexNormal;
	
	// Is normal specified
	// SPCIFIED is not currently used, but may be used in future versions.
	if (rv->rFlags & SPECIFIED_NORMAL)
	{
		vertexNormal = rv->rn.getNormal();
	} else 
	// If normal is not specified it's only available if the face belongs
	// to a smoothing group
	if ((numNormals = rv->rFlags & NORCT_MASK) && smGroup) 
	{
		// If there is only one vertex is found in the rn member.
		if (numNormals == 1) 
		{
			vertexNormal = rv->rn.getNormal();
		} else 
		{
			// If two or more vertices are there you need to step through them
			// and find the vertex with the same smoothing group as the current face.
			// You will find multiple normals in the ern member.
			for (int i = 0; i < numNormals; i++) 
			{
				if (rv->ern[i].getSmGroup() & smGroup) 
					vertexNormal = rv->ern[i].getNormal();
			}
		}

	} else 
		// Get the normal from the Face if no smoothing groups are there
		vertexNormal = mesh->getFaceNormal(faceNo);
	
	return vertexNormal;
}

void Exporter::convertMatrix(Matrix33 &dst, const Matrix3 &src)
{
	Point3 r0 = src.GetRow(0);
	Point3 r1 = src.GetRow(1);
	Point3 r2 = src.GetRow(2);

	dst.Set(r0.x, r0.y, r0.z,
		    r1.x, r1.y, r1.z,
			r2.x, r2.y, r2.z);
}

Matrix3 Exporter::getTransform(INode *node, TimeValue t, bool local)
{
   Matrix3 tm = node->GetObjTMAfterWSM(t);
   if (local)
   {
      Matrix3 pm = node->GetParentTM(t);
      pm.Invert();
      tm *= pm;
   }
   return tm;
}

void Exporter::nodeTransform(Matrix33 &rot, Vector3 &trans, INode *node, TimeValue t, bool local)
{
	Matrix3 tm = getTransform(node, t, local);
	convertMatrix(rot, tm);
	trans.Set(tm.GetTrans().x, tm.GetTrans().y, tm.GetTrans().z);
}

void Exporter::nodeTransform(QuaternionXYZW &rot, Vector3 &trans, INode *node, TimeValue t, bool local)
{
	Matrix33 rm;
	nodeTransform(rm, trans, node, t, local);

	Quaternion q = rm.AsQuaternion();
	rot.x = q.x;
	rot.y = q.y;
	rot.z = q.z;
	rot.w = q.w;
}

bool Exporter::equal(const Vector3 &a, const Point3 &b, float thresh)
{
	return (fabsf(a.x-b.x) <= thresh) &&
		   (fabsf(a.y-b.y) <= thresh) &&
		   (fabsf(a.z-b.z) <= thresh);
}

NiNodeRef Exporter::getNode(const string& name)
{
   NodeMap::iterator itr = mNodeMap.find(name);
   if (itr != mNodeMap.end())
      return (*itr).second;
   NiNodeRef node = CreateNiObject<NiNode>();
   node->SetName(name);
   mNodeMap[name] = node;
   return node;
}

NiNodeRef Exporter::makeNode(NiNodeRef &parent, INode *maxNode, bool local)
{
   string name = (char*)maxNode->GetName();
   NiNodeRef node = getNode(name);

	Matrix33 rot;
	Vector3 trans;
	TimeValue t = 0;
	nodeTransform(rot, trans, maxNode, t, local);
	
	node->SetLocalRotation(rot);
	node->SetLocalTranslation(trans);

   exportUPB(node, maxNode);

	parent->AddChild(DynamicCast<NiAVObject>(node));
	return node;
}

bool Exporter::isCollisionGroup(INode *maxNode, bool root)
{
	if (root)
	{
		if (!maxNode->IsGroupHead())
			return false;
	} else
	{
		if (npIsCollision(maxNode))
			return true;
	}

	for (int i=0; i<maxNode->NumberOfChildren(); i++) 
	{
		if (isCollisionGroup(maxNode->GetChildNode(i), false))
			return true;
	}

	return false;
}

bool Exporter::isMeshGroup(INode *maxNode, bool root)
{
	if (root)
	{
		if (!maxNode->IsGroupHead())
			return false;
	} else
	{
		if (!npIsCollision(maxNode))
		{
			TimeValue t = 0;
			ObjectState os = maxNode->EvalWorldState(t); 
			if (os.obj->SuperClassID() == GEOMOBJECT_CLASS_ID)
				return true;
		}
	}

	for (int i=0; i<maxNode->NumberOfChildren(); i++) 
	{
		if (isMeshGroup(maxNode->GetChildNode(i), false))
			return true;
	}

	return false;
}

bool Exporter::exportUPB(NiNodeRef &root, INode *node)
{
   bool ok = false;
   if (!mUserPropBuffer)
      return ok;

   // Write the actual UPB sans any np_ prefixed strings
   TSTR upb;
   node->GetUserPropBuffer(upb);
   if (!upb.isNull())
   {
      string line;
      istringstream istr(string(upb), ios_base::out);
      ostringstream ostr;
      while (!istr.eof()) {
         std::getline(istr, line);
         if (!line.empty() && 0 != line.compare(0, 3, "np_"))
            ostr << line << endl;
      }
      if (!ostr.str().empty())
      {
         NiStringExtraDataRef strings = CreateNiObject<NiStringExtraData>();	
         strings->SetName("UPB");
         strings->SetData(ostr.str());
         root->AddExtraData(DynamicCast<NiExtraData>(strings));
         ok = true;
      }
   }
   return ok;
}


bool Exporter::removeUnreferencedBones(NiNodeRef node)
{
   NiNodeRef parent = node->GetParent();
   bool remove = (NULL != parent) && !node->IsSkinInfluence();
   Matrix44 ntm = node->GetLocalTransform();
   vector<NiAVObjectRef> children = node->GetChildren();
   for (vector<NiAVObjectRef>::iterator itr = children.begin(); itr != children.end(); ++itr)
   {
      NiAVObjectRef& child = (*itr);
      bool childRemove = false;
      if (child->IsDerivedType(NiNode::TypeConst()))
      {
         childRemove = removeUnreferencedBones(StaticCast<NiNode>(child));
      }
      if (childRemove)
      {
         node->RemoveChild(child);
      }
      else if (remove) // Reparent abandoned nodes to root
      {
         Matrix44 tm = child->GetLocalTransform();
         child->SetLocalTransform( ntm * tm );
         node->RemoveChild(child);
         mNiRoot->AddChild(child);
      }
   }
   return remove;
}

struct SortNodeEquivalence
{
   inline bool operator()(const NiAVObjectRef& lhs, const NiAVObjectRef& rhs) const
   {
      if (!lhs) return !rhs;
      if (!rhs) return true;
      string ltype = lhs->GetType().GetTypeName();
      string rtype = rhs->GetType().GetTypeName();
      if (ltype == rtype)
         return false;
      if (ltype == "bhkCollisionObject")
         return true;
      if (rtype == "bhkCollisionObject")
         return false;
      if (ltype == "NiNode")
         return false;
      else if (rtype == "NiNode")
         return true;
      return (ltype < rtype); 
   }
};

void Exporter::sortNodes(NiNodeRef node)
{
   node->SortChildren(SortNodeEquivalence());

   vector<NiNodeRef> children = DynamicCast<NiNode>(node->GetChildren());
   for (vector<NiNodeRef>::iterator itr = children.begin(); itr != children.end(); ++itr)
      sortNodes(*itr);
}


Exporter::Result Exporter::exportLight(NiNodeRef parent, INode *node, GenLight* light)
{
   TimeValue t = 0;
   NiLightRef niLight;
   switch (light->Type())
   {
   case OMNI_LIGHT:
      {
         if (light->GetAmbientOnly())
         {
            niLight = new NiAmbientLight();
         }
         else
         {
            NiPointLightRef pointLight = new NiPointLight();
            float atten = light->GetAtten(t, ATTEN_START);
            switch (light->GetDecayType())
            {
            case 0: pointLight->SetConstantAttenuation(1.0f); break;
            case 1: pointLight->SetLinearAttenuation( atten / 4.0f ); break;
            case 2: pointLight->SetQuadraticAttenuation( sqrt(atten / 4.0f) ); break;
            }
            niLight = StaticCast<NiLight>(pointLight);
         }
     }
      break;
   case TSPOT_LIGHT:
   case FSPOT_LIGHT:
      niLight = new NiSpotLight();
      break;
   case DIR_LIGHT:
   case TDIR_LIGHT:
      niLight = new NiDirectionalLight();
      break;
   }
   if (niLight == NULL)
      return Skip;

   niLight->SetName(node->GetName());

   Matrix3 tm = getTransform(node, t, !mFlattenHierarchy);
   niLight->SetLocalTransform( TOMATRIX4(tm, false) );

   niLight->SetDimmer( light->GetIntensity(0) );
   Color3 rgbcolor = TOCOLOR3( light->GetRGBColor(0) );
   if (light->GetAmbientOnly())
   {
      niLight->SetDiffuseColor(Color3(0,0,0));
      niLight->SetSpecularColor(Color3(0,0,0));
      niLight->SetAmbientColor(rgbcolor);
   }
   else
   {
      niLight->SetDiffuseColor(rgbcolor);
      niLight->SetSpecularColor(rgbcolor);
      niLight->SetAmbientColor(Color3(0,0,0));
   }
   parent->AddChild( DynamicCast<NiAVObject>(niLight) );
   return Ok;
}

void Exporter::CalcBoundingBox(INode *node, Box3& box, int all)
{
   if (NULL == node) 
      return;

   Matrix3 tm = node->GetObjTMAfterWSM(0);
   if (node->IsBoneShowing()) {
      box.IncludePoints(&tm.GetTrans(), 1, NULL);
   } else {
      if (Object *o = node->GetObjectRef()) {
         if (o->SuperClassID()==GEOMOBJECT_CLASS_ID) {
            if (  o->ClassID() == BONE_OBJ_CLASSID 
               || o->ClassID() == Class_ID(BONE_CLASS_ID,0)
               || o->ClassID() == Class_ID(0x00009125,0) /* Biped Twist Helpers */
               )
            {
               box.IncludePoints(&tm.GetTrans(), 1, NULL);
            }
            else
            {
               Box3 local;
               o->GetLocalBoundBox(0, node, mI->GetActiveViewport(), local);
               box.IncludePoints(&local.Min(), 1, NULL);
               box.IncludePoints(&local.Max(), 1, NULL);
            }
         }
         else if (mExportCameras && o->SuperClassID()==CAMERA_CLASS_ID)
         {
            box.IncludePoints(&tm.GetTrans(), 1, NULL);
         }
      }
   }
   if (all < 0)
      return;

   all = (all>0 ? all : -1);
   for (int i=0; i<node->NumberOfChildren(); i++) {
      CalcBoundingBox(node->GetChildNode(i), box, all );
   }
}

void Exporter::CalcBoundingSphere(INode *node, Point3 center, float& radius, int all)
{
   if (NULL == node) 
      return;

   Matrix3 tm = node->GetObjTMAfterWSM(0);
   Point3 pt = (tm.GetTrans() - center);
   float len = pt.Length();

   if (node->IsBoneShowing()) {
      radius = max(len, radius);
   } else {
      if (Object *o = node->GetObjectRef()) {
         if (o->SuperClassID()==GEOMOBJECT_CLASS_ID) {
            if (  o->ClassID() == BONE_OBJ_CLASSID 
               || o->ClassID() == Class_ID(BONE_CLASS_ID,0)
               || o->ClassID() == Class_ID(0x00009125,0) /* Biped Twist Helpers */
               )
            {
               radius = max(len, radius);
            }
            else
            {
               radius = max(len, radius);
            }
         }
         else if (mExportCameras && o->SuperClassID()==CAMERA_CLASS_ID)
         {
            radius = max(len, radius);
         }
      }
   }
   if (all < 0)
      return;

   all = (all>0 ? all : -1);
   for (int i=0; i<node->NumberOfChildren(); i++) {
      CalcBoundingSphere(node->GetChildNode(i), center, radius, all );
   }
}

