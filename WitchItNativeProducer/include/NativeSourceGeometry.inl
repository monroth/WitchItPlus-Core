static Placement component_placement(UObject* root) {
 using namespace RC::Unreal;pin(root);
 auto vector=[&](const wchar_t* name,std::array<const wchar_t*,3> axes){
  auto* p=CastField<FStructProperty>(field(root->GetClassPrivate(),name,root->GetClassPrivate()->GetStructureSize()));require(p,"source transform field unavailable");
  auto* type=p->GetStruct().Get();void* data=p->ContainerPtrToValuePtr<void>(root);std::array<double,3> values{};
  for(int i=0;i<3;++i){auto* q=field(type,axes[i],type->GetStructureSize());
   if(auto* f=CastField<FFloatProperty>(q)){require(f->GetSize()==sizeof(float));values[i]=*f->ContainerPtrToValuePtr<float>(data);}
   else{auto* d=CastField<FDoubleProperty>(q);require(d && d->GetSize()==sizeof(double));values[i]=*d->ContainerPtrToValuePtr<double>(data);}
   require(std::isfinite(values[i]),"nonfinite cooked transform");}
  return values;
 };
 auto p=vector(L"RelativeLocation",{L"X",L"Y",L"Z"}),r=vector(L"RelativeRotation",{L"Pitch",L"Yaw",L"Roll"}),s=vector(L"RelativeScale3D",{L"X",L"Y",L"Z"});
 return {p[0],p[1],p[2],r[1],r[0],r[2],s[0],s[1],s[2]};
}
