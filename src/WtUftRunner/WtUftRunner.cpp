/*!
 * /file WtUftRunner.cpp
 * /project	WonderTrader
 *
 * /author Wesley
 * /date 2020/03/30
 * 
 * /brief 
 */
#include "WtUftRunner.h"
#include "../WtUftCore/ShareManager.h"

#include "../WtUftCore/WtHelper.h"
#include "../WtUftCore/UftStraContext.h"

#include "../Includes/WTSVariant.hpp"
#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../WTSUtils/SignalHook.hpp"
#include "../Share/StrUtil.hpp"

const char* getBinDir()
{
	static std::string basePath;
	if (basePath.empty())
	{
		basePath = boost::filesystem::initial_path<boost::filesystem::path>().string();

		basePath = StrUtil::standardisePath(basePath);
	}

	return basePath.c_str();
}



WtUftRunner::WtUftRunner()
	:_to_exit(false)
{
	install_signal_hooks([](const char* message) {
		WTSLogger::error(message);
	}, [this](bool bStopped) {
		_to_exit = bStopped;
		WTSLogger::info("Exit flag is {}", _to_exit);
	});
}


WtUftRunner::~WtUftRunner()
{
}

void WtUftRunner::init(const std::string& filename)
{
	WTSLogger::init(filename.c_str());

	WtHelper::setInstDir(getBinDir());
}

/**
 * @brief 配置 WtUftRunner 的运行环境，加载配置文件并初始化各组件
 * 
 * 该函数从指定文件加载配置信息，依据配置内容初始化基础数据、运行引擎、
 * 数据管理、共享域、行情通道、交易通道以及 UFT 策略等组件。
 * 
 * @param filename 配置文件的路径
 * @return bool 配置成功返回 true，失败返回 false
 */
bool WtUftRunner::config(const std::string& filename)
{
	// 从指定文件加载配置信息
	_config = WTSCfgLoader::load_from_file(filename.c_str());
	// 若配置文件加载失败，记录错误日志并返回 false
	if(_config == NULL)
	{
		WTSLogger::error("Loading config file {} failed", filename);
		return false;
	}

	// 基础数据文件配置
	WTSVariant* cfgBF = _config->get("basefiles");
	// 加载交易会话信息
	if (cfgBF->get("session"))
		_bd_mgr.loadSessions(cfgBF->getCString("session"));

	// 加载商品信息
	WTSVariant* cfgItem = cfgBF->get("commodity");
	if (cfgItem)
	{
		// 若配置为字符串类型，直接加载商品信息
		if (cfgItem->type() == WTSVariant::VT_String)
		{
			_bd_mgr.loadCommodities(cfgItem->asCString());
		}
		// 若配置为数组类型，遍历数组加载商品信息
		else if (cfgItem->type() == WTSVariant::VT_Array)
		{
			for (uint32_t i = 0; i < cfgItem->size(); i++)
			{
				_bd_mgr.loadCommodities(cfgItem->get(i)->asCString());
			}
		}
	}

	// 加载合约信息
	cfgItem = cfgBF->get("contract");
	if (cfgItem)
	{
		// 若配置为字符串类型，直接加载合约信息
		if (cfgItem->type() == WTSVariant::VT_String)
		{
			_bd_mgr.loadContracts(cfgItem->asCString());
		}
		// 若配置为数组类型，遍历数组加载合约信息
		else if (cfgItem->type() == WTSVariant::VT_Array)
		{
			for (uint32_t i = 0; i < cfgItem->size(); i++)
			{
				_bd_mgr.loadContracts(cfgItem->get(i)->asCString());
			}
		}
	}

	// 加载节假日信息
	if (cfgBF->get("holiday"))
		_bd_mgr.loadHolidays(cfgBF->getCString("holiday"));

	// 初始化运行引擎
	initEngine();

	// 初始化数据管理
	initDataMgr();

	// 配置共享域
	if (_config->has("share_domain"))
	{
		WTSVariant* cfg = _config->get("share_domain");
		// 设置共享管理器的引擎指针
		ShareManager::self().set_engine(&_uft_engine);
		// 初始化共享管理器模块
		ShareManager::self().initialize(cfg->getCString("module"));
		// 初始化共享域名称
		ShareManager::self().init_domain(cfg->getCString("name"));
	}

	// 初始化操作策略管理器
	if(!_act_policy.init(_config->getCString("bspolicy")))
	{
		// 若初始化失败，记录错误日志
		WTSLogger::error("ActionPolicyMgr init failed, please check config");
	}

	// 初始化行情通道
	WTSVariant* cfgParser = _config->get("parsers");
	if (cfgParser)
	{
		// 若配置为字符串类型，尝试从文件加载行情通道配置
		if (cfgParser->type() == WTSVariant::VT_String)
		{
			const char* filename = cfgParser->asCString();
			if (StdFile::exists(filename))
			{
				WTSLogger::info("Reading parser config from {}...", filename);
				WTSVariant* var = WTSCfgLoader::load_from_file(filename);
				if(var)
				{
					// 若加载成功，初始化行情通道
					if (!initParsers(var->get("parsers")))
						WTSLogger::error("Loading parsers failed");
					var->release();
				}
				else
				{
					// 若加载失败，记录错误日志
					WTSLogger::error("Loading parser config {} failed", filename);
				}
			}
			else
			{
				// 若文件不存在，记录错误日志
				WTSLogger::error("Parser configuration {} not exists", filename);
			}
		}
		// 若配置为数组类型，直接初始化行情通道
		else if (cfgParser->type() == WTSVariant::VT_Array)
		{
			initParsers(cfgParser);
		}
	}

	// 初始化交易通道
	WTSVariant* cfgTraders = _config->get("traders");
	if (cfgTraders)
	{
		// 若配置为字符串类型，尝试从文件加载交易通道配置
		if (cfgTraders->type() == WTSVariant::VT_String)
		{
			const char* filename = cfgTraders->asCString();
			if (StdFile::exists(filename))
			{
				WTSLogger::info("Reading trader config from {}...", filename);
				WTSVariant* var = WTSCfgLoader::load_from_file(filename);
				if (var)
				{
					// 若加载成功，初始化交易通道
					if (!initTraders(var->get("traders")))
						WTSLogger::error("Loading traders failed");
					var->release();
				}
				else
				{
					// 若加载失败，记录错误日志
					WTSLogger::error("Loading trader config {} failed", filename);
				}
			}
			else
			{
				// 若文件不存在，记录错误日志
				WTSLogger::error("Trader configuration {} not exists", filename);
			}
		}
		// 若配置为数组类型，直接初始化交易通道
		else if (cfgTraders->type() == WTSVariant::VT_Array)
		{
			initTraders(cfgTraders);
		}
	}

	// 初始化 UFT 策略
	initUftStrategies();
	
	return true;
}

bool WtUftRunner::initUftStrategies()
{
	WTSVariant* cfg = _config->get("strategies");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Object)
		return false;

	cfg = cfg->get("uft");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Array)
		return false;

	std::string path = WtHelper::getCWD() + "uft/";
	_uft_stra_mgr.loadFactories(path.c_str());

	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
		if(!cfgItem->getBoolean("active"))
			continue;
		const char* id = cfgItem->getCString("id");
		const char* name = cfgItem->getCString("name");
		UftStrategyPtr stra = _uft_stra_mgr.createStrategy(name, id);
		if (stra == NULL)
		{
			WTSLogger::error("UFT Strategy {} create failed", name);
			continue;
		}
		else
		{
			WTSLogger::info("UFT Strategy {}({}) created", name, id);
		}

		stra->self()->init(cfgItem->get("params"));
		UftStraContext* ctx = new UftStraContext(&_uft_engine, id);
		ctx->set_strategy(stra->self());

		const char* traderid = cfgItem->getCString("trader");
		TraderAdapterPtr trader = _traders.getAdapter(traderid);
		if(trader)
		{
			ctx->setTrader(trader.get());
			trader->addSink(ctx);
		}
		else
		{
			WTSLogger::error("Trader {} not exists, binding trader to UFT strategy failed", traderid);
		}

		_uft_engine.addContext(UftContextPtr(ctx));
	}

	return true;
}

bool WtUftRunner::initEngine()
{
	WTSVariant* cfg = _config->get("env");
	if (cfg == NULL)
		return false;

	WTSLogger::info("Trading enviroment initialzied with engine: UFT");
	_uft_engine.init(cfg, &_bd_mgr, &_data_mgr, &_notifier);

	_uft_engine.set_adapter_mgr(&_traders);

	return true;
}


bool WtUftRunner::initDataMgr()
{
	WTSVariant*cfg = _config->get("data");
	if (cfg == NULL)
		return false;

	_data_mgr.init(cfg, &_uft_engine);
	WTSLogger::info("Data manager initialized");

	return true;
}

bool WtUftRunner::initParsers(WTSVariant* cfgParser)
{
	if (cfgParser == NULL)
		return false;

	uint32_t count = 0;
	for (uint32_t idx = 0; idx < cfgParser->size(); idx++)
	{
		WTSVariant* cfgItem = cfgParser->get(idx);
		if(!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		// By Wesley @ 2021.12.14
		// 如果id为空，则生成自动id
		std::string realid = id;
		if (realid.empty())
		{
			static uint32_t auto_parserid = 1000;
			realid = StrUtil::printf("auto_parser_%u", auto_parserid++);
		}

		ParserAdapterPtr adapter(new ParserAdapter);
		adapter->init(realid.c_str(), cfgItem, &_uft_engine, &_bd_mgr);
		_parsers.addAdapter(realid.c_str(), adapter);

		count++;
	}

	WTSLogger::info("{} parsers loaded", count);
	return true;
}

bool WtUftRunner::initTraders(WTSVariant* cfgTrader)
{
	if (cfgTrader == NULL || cfgTrader->type() != WTSVariant::VT_Array)
		return false;
	
	uint32_t count = 0;
	for (uint32_t idx = 0; idx < cfgTrader->size(); idx++)
	{
		WTSVariant* cfgItem = cfgTrader->get(idx);
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");

		TraderAdapterPtr adapter(new TraderAdapter());
		adapter->init(id, cfgItem, &_bd_mgr, &_act_policy);

		_traders.addAdapter(id, adapter);

		count++;
	}

	WTSLogger::info("{} traders loaded", count);

	return true;
}

bool WtUftRunner::initEvtNotifier()
{
	WTSVariant* cfg = _config->get("notifier");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Object)
		return false;

	_notifier.init(cfg);

	return true;
}

void WtUftRunner::run(bool bAsync /* = false */)
{
	try
	{
		_uft_engine.run();

		_parsers.run();
		_traders.run();

		ShareManager::self().start_watching(2);

		while(!_to_exit)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
	catch (...)
	{
		print_stack_trace([](const char* message) {
			WTSLogger::error(message);
		});
	}
}

const char* LOG_TAGS[] = {
	"all",
	"debug",
	"info",
	"warn",
	"error",
	"fatal",
	"none"
};

void WtUftRunner::handleLogAppend(WTSLogLevel ll, const char* msg)
{
}