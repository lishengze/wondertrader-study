/*!
 * \file WtHftTicker.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "WtUftTicker.h"
#include "WtUftEngine.h"
#include "../Includes/IDataReader.h"

#include "../Share/TimeUtils.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/IBaseDataMgr.h"

#include "../WTSTools/WTSLogger.h"

USING_NS_WTP;


WtUftRtTicker::WtUftRtTicker(WtUftEngine* engine)
	: _engine(engine)
	, _stopped(false)
	, _date(0)
	, _time(UINT_MAX)
	, _next_check_time(0)
	, _last_emit_pos(0)
	, _cur_pos(0)
{
}


WtUftRtTicker::~WtUftRtTicker()
{
}

void WtUftRtTicker::init(const char* sessionID)
{
	_s_info = _engine->get_session_info(sessionID);

	TimeUtils::getDateTime(_date, _time);
}

void WtUftRtTicker::on_tick(WTSTickData* curTick)
{
	if (_thrd == NULL)
	{
		if (_engine)
			_engine->on_tick(curTick->code(), curTick);
		return;
	}

	uint32_t uDate = curTick->actiondate();
	uint32_t uTime = curTick->actiontime();

	if (_date != 0 && (uDate < _date || (uDate == _date && uTime < _time)))
	{
		//WTSLogger::info("行情时间{}小于本地时间{}", uTime, _time);
		if (_engine)
			_engine->on_tick(curTick->code(), curTick);
		return;
	}

	_date = uDate;
	_time = uTime;

	uint32_t curMin = _time / 100000;
	uint32_t curSec = _time % 100000;
	uint32_t minutes = _s_info->timeToMinutes(curMin);
	bool isSecEnd = _s_info->isLastOfSection(curMin);
	if (isSecEnd)
	{
		minutes--;
	}
	minutes++;
	uint32_t rawMin = curMin;
	curMin = _s_info->minuteToTime(minutes);

	if (_cur_pos == 0)
	{
		//如果当前时间是0, 则直接赋值即可
		_cur_pos = minutes;
	}
	else if (_cur_pos < minutes)
	{
		//如果已记录的分钟小于新的分钟, 则需要触发闭合事件
		//这个时候要先触发闭合, 再修改平台时间和价格
		if (_last_emit_pos < _cur_pos)
		{
			//触发数据回放模块
			StdUniqueLock lock(_mtx);

			//优先修改时间标记
			_last_emit_pos = _cur_pos;

			uint32_t thisMin = _s_info->minuteToTime(_cur_pos);

			WTSLogger::info("Minute Bar {}.{:04d} Closed by data", _date, thisMin);
			_engine->on_minute_end(_date, thisMin);
		}
			
		if (_engine)
		{
			_engine->on_tick(curTick->code(), curTick);
			_engine->set_date_time(_date, curMin, curSec, rawMin);
			_engine->set_trading_date(curTick->tradingdate());
		}

		_cur_pos = minutes;
	}
	else
	{
		//如果分钟数还是一致的, 则直接触发行情和时间即可
		if (_engine)
		{
			_engine->on_tick(curTick->code(), curTick);
			_engine->set_date_time(_date, curMin, curSec, rawMin);
		}
	}

	uint32_t sec = curSec / 1000;
	uint32_t msec = curSec % 1000;
	uint32_t left_ticks = (60 - sec) * 1000 - msec;
	_next_check_time = TimeUtils::getLocalTimeNow() + left_ticks;
}

/**
 * @brief 启动 WtUftRtTicker 的运行线程
 * 
 * 该函数会初始化一些必要的时间和交易日期信息，然后启动一个新线程，
 * 在新线程中不断检查当前时间，根据交易时间和分钟线闭合情况触发相应事件。
 */
void WtUftRtTicker::run()
{
	// 如果线程已经存在，直接返回，避免重复启动
	if (_thrd)
		return;

	// 调用引擎的初始化回调函数
	_engine->on_init();

	// 计算当前的交易日日期
	uint32_t curTDate = _engine->get_basedata_mgr()->calcTradingDate(_s_info->id(), _engine->get_date(), _engine->get_min_time(), true);
	// 设置引擎的交易日日期
	_engine->set_trading_date(curTDate);

	// 调用引擎的交易会话开始回调函数
	_engine->on_session_begin();

	// 计算当前时间相对于交易时段的偏移时间
	uint32_t offTime = _s_info->offsetTime(_engine->get_min_time(), true);

	// 创建一个新线程
	_thrd.reset(new StdThread([this, offTime](){
		// 当未停止运行时，持续循环
		while (!_stopped)
		{
			// 检查当前时间是否在交易时间内
			if (_time != UINT_MAX && _s_info->isInTradingTime(_time / 100000, true))
			{
				// 短暂休眠 10 毫秒
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				// 获取当前本地时间
				uint64_t now = TimeUtils::getLocalTimeNow();

				// 检查是否到达下一次检查时间，并且上一次触发的位置小于当前位置
				if (now >= _next_check_time && _last_emit_pos < _cur_pos)
				{
					// 加锁，确保线程安全
					StdUniqueLock lock(_mtx);

					// 更新上一次触发的位置为当前位置
					_last_emit_pos = _cur_pos;

					// 将当前位置转换为对应的分钟时间
					uint32_t thisMin = _s_info->minuteToTime(_cur_pos);
					// 更新当前时间
					_time = thisMin;

					// 如果当前分钟时间为 0，说明换日了
					if (thisMin == 0)
					{
						// 记录上一天的日期
						uint32_t lastDate = _date;
						// 获取下一天的日期
						_date = TimeUtils::getNextDate(_date);
						// 将时间置为 0
						_time = 0;
						// 记录日期变更信息
						WTSLogger::info("Data automatically changed at time 00:00: {} -> {}", lastDate, _date);
					}

					// 记录分钟线自动闭合信息
					WTSLogger::info("Minute bar {}.{:04d} closed automatically", _date, thisMin);
					// 调用引擎的分钟线结束回调函数
					_engine->on_minute_end(_date, thisMin);

					// 计算当前分钟时间相对于交易时段的偏移分钟数
					uint32_t offMin = _s_info->offsetTime(thisMin, true);
					// 如果偏移分钟数大于等于交易结束时间，说明交易会话结束
					if (offMin >= _s_info->getCloseTime(true))
					{
						// 调用引擎的交易会话结束回调函数
						_engine->on_session_end();
					}

					// 更新引擎的日期和时间信息
					if (_engine)
						_engine->set_date_time(_date, thisMin, 0);
				}
			}
			else // 当前不在交易时间内
			{
				// 不在交易时间，则休息 10 秒再进行检查
				// 因为这个逻辑是处理分钟线的，所以休盘时间休息 10 秒，不会引起数据踏空的问题
				std::this_thread::sleep_for(std::chrono::seconds(10));
			}
			
		}
	}));
}

void WtUftRtTicker::stop()
{
	_stopped = true;
	if (_thrd)
		_thrd->join();
}
