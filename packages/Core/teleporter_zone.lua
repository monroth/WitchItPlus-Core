-- Cooked B_TeleporterZone geometry. All spawning/reflection stays in Native.
-- Decoded B_Hunter/B_Witch CollisionCylinder defaults: half-height 90 UU.
-- P43 live: floor marker Z20 -> target Z112 matches the incoming standing
-- Hunter Z112. Keep that Z clearance. Native Teleport entry offsets XY beyond
-- the exit/pawn's actual collision footprints, facing only exit yaw.
local M={PAWN_HALF_HEIGHT=90,CLEARANCE=2,CLASS='/Game/Blueprints/StaticProps/B_TeleporterZone.B_TeleporterZone_C'}
local function scene(component,point)
 local location,rotation=component.RelativeLocation,component.RelativeRotation
 location.X=point.X;location.Y=point.Y;location.Z=point.Z
 rotation.Pitch=point.Pitch or 0;rotation.Yaw=point.Yaw or 0;rotation.Roll=point.Roll or 0
 component:SetAbsolute(false,false,false);component:SetAbsolute(true,true,false)
end
function M.configure(actor,entry,destination,options)
 scene(actor.DefaultSceneRoot,entry);scene(actor.TargetLocation,destination)
 local zone=actor.Zone;local extent=zone:GetUnscaledBoxExtent()
 for k,v in pairs(options.extent or {X=100,Y=100,Z=125})do extent[k]=v end
 zone:SetBoxExtent(extent,false)
 actor.UseEffects=options.effects~=false;actor.TransferPropVelocity=options.transferVelocity~=false
 actor.ExitVelocityMultiplier=options.velocity or 1
end
function M.spawn(E,class,world,entry,destination,retain,exitActor)
 local actor,why=E.spawn(class,world,entry,function(a)
  a:SetActorEnableCollision(false);a:SetReplicates(true);a:SetReplicateMovement(true);a.bAlwaysRelevant=true
 end)
 assert(actor,why or 'native teleporter zone spawn refused');retain(actor)
 local zone=actor.Zone;zone:SetGenerateOverlapEvents(false)
 local height=math.max(actor.TargetLocation.RelativeLocation.Z,M.PAWN_HALF_HEIGHT)+M.CLEARANCE
 M.configure(actor,entry,{X=destination.X,Y=destination.Y,Z=destination.Z+height,
  Pitch=0,Yaw=destination.Yaw or 0,Roll=0},{})
 if exitActor then assert(E.teleporter_exit(actor,exitActor),'teleporter landing unavailable') end
 actor.TargetLocation:SetIsReplicated(true)
 actor:SetActorEnableCollision(true);zone:SetGenerateOverlapEvents(true);actor:ForceNetUpdate()
 return actor
end
return M
