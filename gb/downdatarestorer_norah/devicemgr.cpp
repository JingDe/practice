#include"devicemgr.h"
#include"redisclient.h"
#include"mutexlockguard.h"
#include"glog/logging.h"

const string DeviceMgr::s_set_key="gbddrdeviceset";
const string DeviceMgr::s_key_prefix="gbddrdeviceset";

DeviceMgr::DeviceMgr(RedisClient* redis_client, RedisMode redis_mode)
	:redis_client_(redis_client),
	redis_mode_(redis_mode),
//	modify_mutex_(),
	rwmutex_()/*,
    logger_("common.test")*/
{
	LOG(INFO)<<"DeviceMgr constructed";
}

int DeviceMgr::LoadDevices(list<Device>& devices)
{
	ReadGuard guard(rwmutex_);
	
	time_t now=time(NULL);
	list<string> device_id_list;
	redis_client_->smembers(s_set_key, device_id_list);
	for(list<string>::iterator it=device_id_list.begin(); it!=device_id_list.end(); it++)
	{
		Device device;
		if(redis_client_->getSerial(s_key_prefix+*it, device))
		{
			device.UpdateRegisterLastTime(now);
			device.UpdateKeepaliveLastTime(now);
			devices.push_back(device);
		}
	}
	return 0;
}

int DeviceMgr::SearchDevice(const string& device_id, Device& device)
{
	ReadGuard guard(rwmutex_);
	
	bool exist=redis_client_->sismember(s_set_key, device_id);
	if(exist)
	{
		if(redis_client_->getSerial(s_key_prefix+device_id, device))
		{
			return 0;
		}
	}	
	return -1;
}

int DeviceMgr::InsertDevice(const Device& device)
{
	if(redis_mode_==STAND_ALONE_OR_PROXY_MODE)
		return InsertDeviceInTransaction(device);
	else //if(redis_mode_==CLUSTER_MODE)
		return InsertDeviceInCluster(device);
}

int DeviceMgr::InsertDeviceInCluster(const Device& device)
{
//	MutexLockGuard guard(&modify_mutex_);
	WriteGuard guard(rwmutex_);

	if(redis_client_->setSerial(s_key_prefix+device.GetDeviceId(), device)==false)
	{
		return -1;
	}
	
	if(redis_client_->sadd(s_set_key, device.GetDeviceId())==true)
	{
		return 0;
	}
	else
	{
		if(redis_client_->del(s_key_prefix+device.GetDeviceId())==false)
		{
			LOG(ERROR)<<"insert device"<<device.GetDeviceId()<<" failed, undo set failed";
		}
		return -1;
	}
}

int DeviceMgr::InsertDeviceInTransaction(const Device& device)
{
    //     MutexLockGuard guard(&modify_mutex_);
    WriteGuard guard(rwmutex_);

    RedisConnection *con=NULL;

    if(!redis_client_->PrepareTransaction(&con))
    {
        //        logger_.error("PrepareTransaction failed");
        LOG(ERROR)<<"PrepareTransaction failed";
        goto FAIL;
    }
    if(!redis_client_->StartTransaction(con))
    {
        //        logger_.error("StartTransaction failed");
        LOG(ERROR)<<"StartTransaction failed";
        goto FAIL;
    }
    if(!redis_client_->Set(con, s_key_prefix+device.GetDeviceId(), device))
    {
        LOG(ERROR)<<"Set failed";
        goto FAIL;
    }
    if(!redis_client_->Sadd(con, s_set_key, device.GetDeviceId()))
    {
        LOG(ERROR)<<"Sadd failed";
        goto FAIL;
    }
    if(!redis_client_->ExecTransaction(con))
    {
        LOG(ERROR)<<"ExecTransaction failed";
        goto FAIL;
    }
    redis_client_->FinishTransaction(&con);
    LOG(INFO)<<"InsertDevice success: "<<device.GetDeviceId();
    return 0;

FAIL:
    if(con)
    {
        redis_client_->FinishTransaction(&con);
    }
    LOG(ERROR)<<"InsertDevice failed: "<<device.GetDeviceId();
    return -1;
}

int DeviceMgr::DeleteDevice(const Device& device)
{
	if(redis_mode_==STAND_ALONE_OR_PROXY_MODE)
		return DeleteDeviceInTransaction(device);
	else //if(redis_mode_==CLUSTER_MODE)
		return DeleteDeviceInCluster(device);
}

int DeviceMgr::DeleteDeviceInCluster(const Device& device)
{
//	MutexLockGuard guard(&modify_mutex_);
	WriteGuard guard(rwmutex_);

	if(redis_client_->del(s_key_prefix+device.GetDeviceId())==false)
		return -1;
	
	if(redis_client_->srem(s_set_key, device.GetDeviceId())==true)
	{
		LOG(INFO)<<"delete device "<<device.GetDeviceId()<<" ok";
		return 0;
	}
	else
	{
		if(redis_client_->setSerial(s_key_prefix+device.GetDeviceId(), device)==false)
		{
			LOG(ERROR)<<"delete device"<<device.GetDeviceId()<<" failed, undo del failed";
		}
		return -1;
	}
}


int DeviceMgr::DeleteDeviceInTransaction(const Device& device)
{
//	MutexLockGuard guard(&modify_mutex_);
	WriteGuard guard(rwmutex_);

	RedisConnection *con=NULL;

    if(!redis_client_->PrepareTransaction(&con))
    {
    	LOG(ERROR)<<"PrepareTransaction failed";
    	goto FAIL;
    }
    if(!redis_client_->StartTransaction(con))
    {
	    LOG(ERROR)<<"StartTransaction failed";
    	goto FAIL;
    }
    if(!redis_client_->Del(con, s_key_prefix+device.GetDeviceId()))
    {
	    LOG(ERROR)<<"Del failed";
    	goto FAIL;
    }
    if(!redis_client_->Srem(con, s_set_key, device.GetDeviceId()))
    {
	    LOG(ERROR)<<"Srem failed";
    	goto FAIL;
    }
    if(!redis_client_->ExecTransaction(con))
    {
	    LOG(ERROR)<<"ExecTransaction failed";
    	goto FAIL;
    }
            
    redis_client_->FinishTransaction(&con);
    LOG(INFO)<<"DeleteDevice success: "<<device.GetDeviceId();
	return 0;

FAIL:
    if(con)
    {                                                                            
        redis_client_->FinishTransaction(&con);
    }
    LOG(ERROR)<<"DeleteDevice failed: "<<device.GetDeviceId();
    return -1;
}

int DeviceMgr::ClearDevices()
{
//	MutexLockGuard guard(&modify_mutex_);
	WriteGuard guard(rwmutex_);
	
	return ClearDevicesWithLockHeld();
}

int DeviceMgr::ClearDevicesWithLockHeld()
{	
	list<string> device_id_list;
	redis_client_->smembers(s_set_key, device_id_list);
	redis_client_->del(s_set_key);
	for(list<string>::iterator it=device_id_list.begin(); it!=device_id_list.end(); it++)
	{
		redis_client_->del(s_key_prefix+*it);
	}
	return 0;
}



int DeviceMgr::UpdateDevices(const list<Device>& devices)
{
//	MutexLockGuard guard(&modify_mutex_);
	WriteGuard guard(rwmutex_);

	ClearDevicesWithLockHeld();
	
	for(list<Device>::const_iterator it=devices.begin(); it!=devices.end(); ++it)
	{
		redis_client_->sadd(s_set_key, it->GetDeviceId());
		redis_client_->setSerial(s_key_prefix+it->GetDeviceId(), *it);
	}
    return 0;
}

int DeviceMgr::UpdateDevices(const list<void*>& devices)
{
//	MutexLockGuard guard(&modify_mutex_);
	WriteGuard guard(rwmutex_);

	ClearDevicesWithLockHeld();

	for(list<void*>::const_iterator it=devices.begin(); it!=devices.end(); ++it)
	{
		Device* device=(Device*)(*it);
		redis_client_->sadd(s_set_key, device->GetDeviceId());
		redis_client_->setSerial(s_key_prefix+device->GetDeviceId(), *device);
	}
    return 0;
}

int DeviceMgr::GetDeviceCount()
{
	ReadGuard guard(rwmutex_);
	return redis_client_->scard(s_set_key);
}
