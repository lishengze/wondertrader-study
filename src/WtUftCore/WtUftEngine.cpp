/*!
 * \file WtHftEngine.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#define WIN32_LEAN_AND_MEAN

#include "WtUftEngine.h"
#include "WtUftTicker.h"
#include "WtUftDtMgr.h"
#include "TraderAdapter.h"
#include "WtHelper.h"

#include "../Share/decimal.h"
#include "../Share/StrUtil.hpp"
#include "../Share/TimeUtils.hpp"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSContractInfo.hpp"

#include "../WTSTools/WTSLogger.h"

USING_NS_WTP;

/**
 * @brief WtUftEngine 类的构造函数，用于初始化引擎的基本属性和时间信息
 */
WtUftEngine::WtUftEngine()
	: _cfg(NULL)  // 初始化配置信息指针为 NULL
	, _tm_ticker(NULL)  // 初始化时间 ticker 指针为 NULL
	, _notifier(NULL)  // 初始化事件通知器指针为 NULL
{
	// 调用 TimeUtils 工具类的 getDateTime 方法，获取当前的日期和时间
	// 日期存储在 _cur_date 中，时间存储在 _cur_time 中
	TimeUtils::getDateTime(_cur_date, _cur_time);

	// 从 _cur_time 中提取出精确到毫秒的秒数信息
	_cur_secs = _cur_time % 100000;

	// 去除 _cur_time 中的毫秒部分，得到 HHMMSS 格式的时间
	_cur_time /= 100000;

	// 将处理后的时间赋值给原始时间
	_cur_raw_time = _cur_time;

	// 将当前日期赋值给交易日日期
	_cur_tdate = _cur_date;

	// 调用 WtHelper 工具类的 setTime 方法，设置当前的日期、时间和秒数信息
	WtHelper::setTime(_cur_date, _cur_time, _cur_secs);
}


WtUftEngine::~WtUftEngine()
{
	if (_tm_ticker)
	{
		_tm_ticker->stop();
		delete _tm_ticker;
		_tm_ticker = NULL;
	}

	if (_cfg)
		_cfg->release();
}

/**
 * @brief 设置当前的日期、时间和秒数信息，并更新辅助工具中的时间信息
 * 
 * 该函数用于更新 WtUftEngine 实例的日期、时间和秒数信息，
 * 同时会将更新后的时间信息同步到 WtHelper 工具类中。
 * 
 * @param curDate 当前日期，格式为 YYYYMMDD
 * @param curTime 当前时间，格式为 HHMMSS，不包含毫秒
 * @param curSecs 当前秒数，精确到毫秒，默认为 0
 * @param rawTime 原始时间，格式为 HHMMSS，默认为 0，若为 0 则使用 curTime 的值
 */
void WtUftEngine::set_date_time(uint32_t curDate, uint32_t curTime, uint32_t curSecs /* = 0 */, uint32_t rawTime /* = 0 */)
{
	// 更新当前日期
	_cur_date = curDate;
	// 更新当前时间
	_cur_time = curTime;
	// 更新当前秒数
	_cur_secs = curSecs;

	// 若原始时间未指定（值为 0），则使用当前时间作为原始时间
	if (rawTime == 0)
		rawTime = curTime;

	// 更新原始时间
	_cur_raw_time = rawTime;

	// 将更新后的日期、原始时间和秒数信息同步到 WtHelper 工具类中
	WtHelper::setTime(_cur_date, _cur_raw_time, _cur_secs);
}

void WtUftEngine::set_trading_date(uint32_t curTDate)
{
	_cur_tdate = curTDate;

	WtHelper::setTDate(curTDate);
}

/**
 * @brief 根据标准合约代码获取商品信息
 * 
 * 该函数接收一个标准合约代码，通过分割该代码获取交易所代码和合约代码，
 * 然后从基础数据管理器中获取对应的合约信息，最后从合约信息中获取商品信息。
 * 如果在过程中无法获取到合约信息，则返回 NULL。
 * 
 * @param stdCode 标准合约代码，格式为 "交易所代码.合约代码"
 * @return WTSCommodityInfo* 商品信息指针，如果获取失败则返回 NULL
 */
WTSCommodityInfo* WtUftEngine::get_commodity_info(const char* stdCode)
{
	// 使用 StrUtil::split 函数按 '.' 分割标准合约代码，结果存储在 ay 中
	const StringVector& ay = StrUtil::split(stdCode, ".");
	// 从基础数据管理器中获取合约信息，参数为交易所代码和合约代码
	WTSContractInfo* cInfo = _base_data_mgr->getContract(ay[1].c_str(), ay[0].c_str());
	// 如果未获取到合约信息，返回 NULL
	if (cInfo == NULL)
		return NULL;

	// 从合约信息中获取商品信息并返回
	return cInfo->getCommInfo();
}

/**
 * @brief 根据标准合约代码获取合约信息
 * 
 * 该函数接收一个标准合约代码，通过分割该代码获取交易所代码和合约代码，
 * 然后从基础数据管理器中获取对应的合约信息。
 * 
 * @param stdCode 标准合约代码，格式为 "交易所代码.合约代码"
 * @return WTSContractInfo* 合约信息指针，如果获取失败可能返回 NULL
 */
WTSContractInfo* WtUftEngine::get_contract_info(const char* stdCode)
{
	// 使用 StrUtil::split 函数按 '.' 分割标准合约代码，结果存储在 ay 中
	// ay[0] 为交易所代码，ay[1] 为合约代码
	const StringVector& ay = StrUtil::split(stdCode, ".");
	// 从基础数据管理器中获取合约信息，参数为合约代码和交易所代码
	return _base_data_mgr->getContract(ay[1].c_str(), ay[0].c_str());
}

/**
 * @brief 根据会话 ID 或合约代码获取会话信息
 * 
 * 该函数根据传入的标识符和标志位判断是直接获取会话信息，
 * 还是先通过合约代码获取合约信息，再从合约信息中获取商品信息，
 * 最后从商品信息中获取会话信息。
 * 
 * @param sid 会话 ID 或标准合约代码，格式为 "交易所代码.合约代码"
 * @param isCode 标志位，指示 sid 是否为合约代码，默认为 false
 * @return WTSSessionInfo* 会话信息指针，如果获取失败则返回 NULL
 */
WTSSessionInfo* WtUftEngine::get_session_info(const char* sid, bool isCode /* = false */)
{
	// 如果 sid 不是合约代码，直接从基础数据管理器中获取会话信息
	if (!isCode)
		return _base_data_mgr->getSession(sid);

	// 如果 sid 是合约代码，使用 StrUtil::split 函数按 '.' 分割合约代码
	const StringVector& ay = StrUtil::split(sid, ".");
	// 从基础数据管理器中获取合约信息，参数为合约代码和交易所代码
	WTSContractInfo* cInfo = _base_data_mgr->getContract(ay[1].c_str(), ay[0].c_str());
	// 如果未获取到合约信息，返回 NULL
	if (cInfo == NULL)
		return NULL;

	// 从合约信息中获取商品信息
	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	// 从商品信息中获取会话信息并返回
	return commInfo->getSessionInfo();
}

/**
 * @brief 获取指定合约的 tick 数据切片
 * 
 * 该函数尝试从数据管理器中获取指定合约的 tick 数据切片。
 * 不过当前代码存在逻辑问题，会先返回 NULL，后续代码无法执行。
 * 
 * @param sid 会话 ID
 * @param code 合约代码
 * @param count 需要获取的 tick 数据数量
 * @return WTSTickSlice* tick 数据切片指针，如果获取失败或代码逻辑有误可能返回 NULL
 */
WTSTickSlice* WtUftEngine::get_tick_slice(uint32_t sid, const char* code, uint32_t count)
{
	// 此处直接返回 NULL，导致后续代码无法执行，存在逻辑问题
	return NULL;
	// 尝试从数据管理器中获取指定合约的 tick 数据切片
	return _data_mgr->get_tick_slice(code, count);
}

WTSTickData* WtUftEngine::get_last_tick(uint32_t sid, const char* stdCode)
{
	return _data_mgr->grab_last_tick(stdCode);
}

/**
 * @brief 获取指定合约的 K 线数据切片
 * 
 * 该函数尝试获取指定合约在特定周期下的 K 线数据切片。
 * 不过当前代码存在逻辑问题，会先返回 NULL，后续代码无法执行。
 * 
 * @param sid 会话 ID
 * @param stdCode 标准合约代码，格式为 "交易所代码.合约代码"
 * @param period 周期类型，如 "m" 表示分钟线，其他值表示日线
 * @param count 需要获取的 K 线数据数量
 * @param times 周期倍数，默认为 1
 * @param etime 结束时间，默认为 0
 * @return WTSKlineSlice* K 线数据切片指针，如果获取失败或代码逻辑有误可能返回 NULL
 */
WTSKlineSlice* WtUftEngine::get_kline_slice(uint32_t sid, const char* stdCode, const char* period, uint32_t count, uint32_t times /* = 1 */, uint64_t etime /* = 0 */)
{
	// 此处直接返回 NULL，导致后续代码无法执行，存在逻辑问题
	return NULL;

	// 从基础数据管理器中获取商品信息
	WTSCommodityInfo* cInfo = _base_data_mgr->getCommodity(stdCode);
	// 如果未获取到商品信息，返回 NULL
	if (cInfo == NULL)
		return NULL;

	// 从商品信息中获取会话信息
	WTSSessionInfo* sInfo = cInfo->getSessionInfo();

	// 生成 K 线订阅的键，格式为 "合约代码-周期类型-周期倍数"
	std::string key = fmt::format("{}-{}-{}", stdCode, period, times);
	// 获取该键对应的会话 ID 列表引用
	SubList& sids = _bar_sub_map[key];
	// 将当前会话 ID 插入到会话 ID 列表中
	sids.insert(sid);

	// 定义 K 线周期类型变量
	WTSKlinePeriod kp;
	// 判断周期类型是否为分钟线
	if (strcmp(period, "m") == 0)
	{
		// 如果周期倍数是 5 的倍数
		if (times % 5 == 0)
		{
			// 设置 K 线周期类型为 5 分钟线
			kp = KP_Minute5;
			// 周期倍数除以 5
			times /= 5;
		}
		else
			// 否则设置 K 线周期类型为 1 分钟线
			kp = KP_Minute1;
	}
	else
	{
		// 其他情况设置 K 线周期类型为日线
		kp = KP_DAY;
	}

	// 从数据管理器中获取 K 线数据切片并返回
	return _data_mgr->get_kline_slice(stdCode, kp, times, count, etime);
}

void WtUftEngine::sub_tick(uint32_t sid, const char* stdCode)
{
	SubList& sids = _tick_sub_map[stdCode];
	sids.insert(sid);
}

double WtUftEngine::get_cur_price(const char* stdCode)
{
	WTSTickData* lastTick = _data_mgr->grab_last_tick(stdCode);
	if (lastTick == NULL)
		return 0.0;

	double ret = lastTick->price();
	lastTick->release();
	return ret;
}

void WtUftEngine::notify_params_update(const char* name)
{
	for(auto& v : _ctx_map)
	{
		const UftContextPtr& context = v.second;
		if(strcmp(context->name(), name) == 0)
		{
			context->on_params_updated();
			break;
		}
	}
}

void WtUftEngine::init(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtUftDtMgr* dataMgr, EventNotifier* notifier)
{
	_base_data_mgr = bdMgr;
	_data_mgr = dataMgr;
	_notifier = notifier;

	_cfg = cfg;
	if(_cfg) _cfg->retain();
}

void WtUftEngine::run()
{
	for (auto it = _ctx_map.begin(); it != _ctx_map.end(); it++)
	{
		UftContextPtr& ctx = (UftContextPtr&)it->second;
		ctx->on_init();
	}

	_tm_ticker = new WtUftRtTicker(this);
	if(_cfg && _cfg->has("product"))
	{
		WTSVariant* cfgProd = _cfg->get("product");
		_tm_ticker->init(cfgProd->getCString("session"));
	}
	else
	{
		_tm_ticker->init("ALLDAY");
	}

	_tm_ticker->run();
}

void WtUftEngine::handle_push_quote(WTSTickData* newTick)
{
	if (_tm_ticker)
		_tm_ticker->on_tick(newTick);
}

void WtUftEngine::handle_push_order_detail(WTSOrdDtlData* curOrdDtl)
{
	const char* stdCode = curOrdDtl->code();
	auto sit = _orddtl_sub_map.find(stdCode);
	if (sit != _orddtl_sub_map.end())
	{
		const SubList& sids = sit->second;
		for (auto it = sids.begin(); it != sids.end(); it++)
		{
			//By Wesley @ 2022.02.07
			//Level2数据一般用于HFT场景，所以不做复权处理
			//所以不读取订阅标记
			uint32_t sid = *it;
			auto cit = _ctx_map.find(sid);
			if (cit != _ctx_map.end())
			{
				UftContextPtr& ctx = (UftContextPtr&)cit->second;
				ctx->on_order_detail(stdCode, curOrdDtl);
			}
		}
	}
}

void WtUftEngine::handle_push_order_queue(WTSOrdQueData* curOrdQue)
{
	const char* stdCode = curOrdQue->code();
	auto sit = _ordque_sub_map.find(stdCode);
	if (sit != _ordque_sub_map.end())
	{
		const SubList& sids = sit->second;
		for (auto it = sids.begin(); it != sids.end(); it++)
		{
			//By Wesley @ 2022.02.07
			//Level2数据一般用于HFT场景，所以不做复权处理
			//所以不读取订阅标记
			uint32_t sid = *it;
			auto cit = _ctx_map.find(sid);
			if (cit != _ctx_map.end())
			{
				UftContextPtr& ctx = (UftContextPtr&)cit->second;
				ctx->on_order_queue(stdCode, curOrdQue);
			}
		}
	}
}

void WtUftEngine::handle_push_transaction(WTSTransData* curTrans)
{
	const char* stdCode = curTrans->code();
	auto sit = _trans_sub_map.find(stdCode);
	if (sit != _trans_sub_map.end())
	{
		const SubList& sids = sit->second;
		for (auto it = sids.begin(); it != sids.end(); it++)
		{
			//By Wesley @ 2022.02.07
			//Level2数据一般用于HFT场景，所以不做复权处理
			//所以不读取订阅标记
			uint32_t sid = *it;
			auto cit = _ctx_map.find(sid);
			if (cit != _ctx_map.end())
			{
				UftContextPtr& ctx = (UftContextPtr&)cit->second;
				ctx->on_transaction(stdCode, curTrans);
			}
		}
	}
}

void WtUftEngine::sub_order_detail(uint32_t sid, const char* stdCode)
{
	SubList& sids = _orddtl_sub_map[stdCode];
	sids.insert(sid);
}

void WtUftEngine::sub_order_queue(uint32_t sid, const char* stdCode)
{
	SubList& sids = _ordque_sub_map[stdCode];
	sids.insert(sid);
}

void WtUftEngine::sub_transaction(uint32_t sid, const char* stdCode)
{
	SubList& sids = _trans_sub_map[stdCode];
	sids.insert(sid);
}

void WtUftEngine::on_session_begin()
{
	WTSLogger::info("Trading day {} begun", _cur_tdate);

	for (auto it = _ctx_map.begin(); it != _ctx_map.end(); it++)
	{
		UftContextPtr& ctx = (UftContextPtr&)it->second;
		ctx->on_session_begin(_cur_tdate);
	}
}

void WtUftEngine::on_session_end()
{
	for (auto it = _ctx_map.begin(); it != _ctx_map.end(); it++)
	{
		UftContextPtr& ctx = (UftContextPtr&)it->second;
		ctx->on_session_end(_cur_tdate);
	}

	WTSLogger::info("Trading day {} ended", _cur_tdate);
}

void WtUftEngine::on_tick(const char* stdCode, WTSTickData* curTick)
{
	if(_data_mgr)
		_data_mgr->handle_push_quote(stdCode, curTick);

	{
		auto sit = _tick_sub_map.find(stdCode);
		if (sit != _tick_sub_map.end())
		{
			const SubList& sids = sit->second;
			for (auto it = sids.begin(); it != sids.end(); it++)
			{
				uint32_t sid = *it;

				auto cit = _ctx_map.find(sid);
				if (cit != _ctx_map.end())
				{
					UftContextPtr& ctx = (UftContextPtr&)cit->second;
					ctx->on_tick(stdCode, curTick);
				}
			}
		}
	}
}

void WtUftEngine::on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	std::string key = fmt::format("{}-{}-{}", stdCode, period, times);
	const SubList& sids = _bar_sub_map[key];
	for (auto it = sids.begin(); it != sids.end(); it++)
	{
		uint32_t sid = *it;
		auto cit = _ctx_map.find(sid);
		if (cit != _ctx_map.end())
		{
			UftContextPtr& ctx = (UftContextPtr&)cit->second;
			ctx->on_bar(stdCode, period, times, newBar);
		}
	}
}

void WtUftEngine::on_minute_end(uint32_t curDate, uint32_t curTime)
{

}

void WtUftEngine::addContext(UftContextPtr ctx)
{
	uint32_t sid = ctx->id();
	_ctx_map[sid] = ctx;
}

UftContextPtr WtUftEngine::getContext(uint32_t id)
{
	auto it = _ctx_map.find(id);
	if (it == _ctx_map.end())
		return UftContextPtr();

	return it->second;
}

WTSOrdQueSlice* WtUftEngine::get_order_queue_slice(uint32_t sid, const char* code, uint32_t count)
{
	return _data_mgr->get_order_queue_slice(code, count);
}

WTSOrdDtlSlice* WtUftEngine::get_order_detail_slice(uint32_t sid, const char* code, uint32_t count)
{
	return _data_mgr->get_order_detail_slice(code, count);
}

WTSTransSlice* WtUftEngine::get_transaction_slice(uint32_t sid, const char* code, uint32_t count)
{
	return _data_mgr->get_transaction_slice(code, count);
}