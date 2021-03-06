#include "OeipManager.h"
#include "VideoManager.h"


OeipManager* OeipManager::instance = nullptr;

void cleanPlugin(bool bFactory) {
	PluginManager<VideoManager>::clean(bFactory);
	PluginManager<ImageProcess>::clean(bFactory);
	PluginManager<AudioRecord>::clean(bFactory);
	PluginManager<AudioOutput>::clean(bFactory);
}

OeipManager* OeipManager::getInstance() {
	if (instance == nullptr) {
		instance = new OeipManager();
	}
	return instance;
}

void OeipManager::shutdown() {
	safeDelete(instance);
#ifdef _DEBUG
	cleanPlugin(true);
#else
	cleanPlugin(false);
#endif
}

OeipManager::~OeipManager() {
	std::unique_lock <std::mutex> mtx_locker(mtx, std::try_to_lock);
	//通过PluginManager生成的对象,在这不删除,后面通过PluginManager自身来删除
	for (auto& video : videoList) {
		video->closeDevice();
	}
	for (auto& pipe : imagePipeList) {
		pipe->closePipe();
	}
	videoList.clear();
	imagePipeList.clear();
}

OeipManager::OeipManager() {
	initVideoList();
	audioOutput = PluginManager<AudioOutput>::getInstance().createModel(0);
	if (audioOutput) {

	}
}

void OeipManager::initVideoList() {
	videoList.clear();
	std::vector<VideoManager*> vmlist;
	PluginManager<VideoManager>::getInstance().getFactoryDefaultModel(vmlist, -1);
	for (auto& vm : vmlist) {
		std::vector<VideoDevice*> deviceList = vm->getDeviceList();
		videoList.insert(videoList.end(), deviceList.begin(), deviceList.end());
	}
}

int32_t OeipManager::initPipe(OeipGpgpuType gpgpuType) {
	std::unique_lock <std::mutex> mtx_locker(mtx, std::try_to_lock);
	//先检查是否有已经不用的管线
	std::vector<ImageProcess*> pipeList;
	PluginManager<ImageProcess>::getInstance().getModelList(pipeList, gpgpuType);
	for (int32_t i = 0; i < pipeList.size(); i++) {
		if (pipeList[i]->emptyPipe())
			return i;
	}
	//如果没有就申请个新的
	auto vp = PluginManager<ImageProcess>::getInstance().createModel(gpgpuType);
	if (vp == nullptr)
		return -1;
	vp->gpuType = gpgpuType;
	imagePipeList.push_back(vp);
	return imagePipeList.size() - 1;
}

bool OeipManager::closePipe(int32_t pipeId) {
	std::unique_lock <std::mutex> mtx_locker(mtx, std::try_to_lock);
	if (pipeId < 0 || pipeId >= imagePipeList.size())
		return -1;
	//关闭相应管线
	imagePipeList[pipeId]->closePipe();
}

int32_t OeipManager::initReadMedia() {
	std::unique_lock <std::mutex> mtx_locker(mtx, std::try_to_lock);
	std::vector<MediaPlay*> playList;
	PluginManager<MediaPlay>::getInstance().getModelList(playList, -1);
	for (int32_t i = 0; i < playList.size(); i++) {
		if (!playList[i]->bOpen())
			return i;
	}
	auto selectPaly = PluginManager<MediaPlay>::getInstance().createModel(0);
	if (selectPaly == nullptr)
		return -1;
	mediaPlayList.push_back(selectPaly);
	return mediaPlayList.size() - 1;
}

int32_t OeipManager::initWriteMedia() {
	std::unique_lock <std::mutex> mtx_locker(mtx, std::try_to_lock);
	std::vector<MediaOutput*> playList;
	PluginManager<MediaOutput>::getInstance().getModelList(playList, -1);
	for (int32_t i = 0; i < playList.size(); i++) {
		if (!playList[i]->bOpen())
			return i;
	}
	auto selectPaly = PluginManager<MediaOutput>::getInstance().createModel(0);
	if (selectPaly == nullptr)
		return -1;
	mediaOutputList.push_back(selectPaly);
	return mediaOutputList.size() - 1;
}




