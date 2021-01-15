/*!
 * \file WtSimpExeUnit.cpp
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * 
 */
#include "WtSimpExeUnit.h"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Share/decimal.h"

extern const char* FACT_NAME;


WtSimpExeUnit::WtSimpExeUnit()
	: _last_tick(NULL)
	, _comm_info(NULL)
	, _price_mode(0)
	, _price_offset(0)
	, _expire_secs(0)
	, _cancel_cnt(0)
	, _target_pos(0)
	, _unsent_qty(0)
	, _cancel_times(0)
{
}


WtSimpExeUnit::~WtSimpExeUnit()
{
	if (_last_tick)
		_last_tick->release();

	if (_comm_info)
		_comm_info->release();
}

const char* WtSimpExeUnit::getFactName()
{
	return FACT_NAME;
}

const char* WtSimpExeUnit::getName()
{
	return "WtSimpExeUnit";
}

const char* PriceModeNames[] = 
{
	"最优价",
	"最新价",
	"对手价",
	"自适应"
};
void WtSimpExeUnit::init(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg)
{
	ExecuteUnit::init(ctx, stdCode, cfg);

	_comm_info = ctx->getCommodityInfo(stdCode);
	if (_comm_info)
		_comm_info->retain();

	_sess_info = ctx->getSessionInfo(stdCode);
	if (_sess_info)
		_sess_info->retain();

	_price_offset = cfg->getInt32("offset");
	_expire_secs = cfg->getUInt32("expire");
	_price_mode = cfg->getInt32("pricemode");	//价格类型，0-最新价，-1-最优价，1-对手价，2-自动，默认为0

	ctx->writeLog("执行单元 %s 初始化完成，委托价 %s ± %d 跳，订单超时 %u 秒", stdCode, PriceModeNames[_price_mode+1], _price_offset, _expire_secs);
}

void WtSimpExeUnit::on_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled)
{
	{
		if (!_orders_mon.has_order(localid))
			return;

		if (isCanceled || leftover == 0)
		{
			_orders_mon.erase_order(localid);
			if (_cancel_cnt > 0)
				_cancel_cnt--;

			_ctx->writeLog("@ %d cancelcnt -> %u", __LINE__, _cancel_cnt);
		}

		if (leftover == 0 && !isCanceled)
			_cancel_times = 0;
	}

	//如果有撤单，也触发重新计算
	if (isCanceled)
	{
		_ctx->writeLog("%s的订单%u已撤销，重新触发执行逻辑", stdCode, localid);
		_cancel_times++;
		doCalculate();
	}
}

void WtSimpExeUnit::on_channel_ready()
{
	double undone = _ctx->getUndoneQty(_code.c_str());

	if(!decimal::eq(undone, 0) && !_orders_mon.has_order())
	{
		//这说明有未完成单不在监控之中，先撤掉
		_ctx->writeLog("%s有不在管理中的未完成单 %f ，全部撤销", _code.c_str(), undone);

		bool isBuy = (undone > 0);
		OrderIDs ids = _ctx->cancel(_code.c_str(), isBuy);
		_orders_mon.push_order(ids.data(), ids.size(), _ctx->getCurTime());
		_cancel_cnt += ids.size();

		_ctx->writeLog("%s cancelcnt -> %u", __FUNCTION__, _cancel_cnt);
	}


	doCalculate();
}

void WtSimpExeUnit::on_channel_lost()
{
	
}

void WtSimpExeUnit::on_tick(WTSTickData* newTick)
{
	if (newTick == NULL || _code.compare(newTick->code()) != 0)
		return;

	bool isFirstTick = false;
	//如果原来的tick不为空，则要释放掉
	if (_last_tick)
	{
		_last_tick->release();
	}
	else
	{
		//如果行情时间不在交易时间，这种情况一般是集合竞价的行情进来，下单会失败，所以直接过滤掉这笔行情
		if (_sess_info != NULL && !_sess_info->isInTradingTime(newTick->actiontime() / 100000))
			return;

		isFirstTick = true;
	}

	//新的tick数据，要保留
	_last_tick = newTick;
	_last_tick->retain();

	/*
	 *	这里可以考虑一下
	 *	如果写的上一次丢出去的单子不够达到目标仓位
	 *	那么在新的行情数据进来的时候可以再次触发核心逻辑
	 */

	if(isFirstTick)	//如果是第一笔tick，则检查目标仓位，不符合则下单
	{
		double newVol = _target_pos;
		const char* stdCode = _code.c_str();

		double undone = _ctx->getUndoneQty(stdCode);
		double realPos = _ctx->getPosition(stdCode);

		if (!decimal::eq(newVol, undone + realPos))
		{
			doCalculate();
		}
	}
	else if(_expire_secs != 0 && _orders_mon.has_order() && _cancel_cnt==0)
	{
		uint64_t now = _ctx->getCurTime();

		_orders_mon.check_orders(_expire_secs, now, [this](uint32_t localid) {
			if (_ctx->cancel(localid))
			{
				_cancel_cnt++;
				_ctx->writeLog("@ %d cancelcnt -> %u", __LINE__, _cancel_cnt);
			}
		});
	}
}

void WtSimpExeUnit::on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
	//不用触发，这里在ontick里触发吧
}

void WtSimpExeUnit::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message)
{
	if (!bSuccess)
	{
		//如果不是我发出去的订单，我就不管了
		if (!_orders_mon.has_order(localid))
			return;

		_orders_mon.erase_order(localid);

		doCalculate();
	}
}

void WtSimpExeUnit::doCalculate()
{
	if (_cancel_cnt != 0)
		return;

	double newVol = _target_pos;
	const char* stdCode = _code.c_str();

	double undone = _ctx->getUndoneQty(stdCode);
	double realPos = _ctx->getPosition(stdCode);

	//如果有反向未完成单，则直接撤销
	//如果目标仓位为0，且当前持仓为0，则撤销全部挂单
	if (decimal::lt(newVol * undone, 0))
	{
		bool isBuy = decimal::gt(undone, 0);
		OrderIDs ids = _ctx->cancel(stdCode, isBuy);
		_orders_mon.push_order(ids.data(), ids.size(), _ctx->getCurTime());
		_cancel_cnt += ids.size();
		_ctx->writeLog("@ %d cancelcnt -> %u", __LINE__, _cancel_cnt);
		return;
	}
	//else if(newVol == 0 && undone != 0)
	else if (decimal::eq(newVol,0) && !decimal::eq(undone, 0))
	{
		//如果目标仓位为0，且未完成不为0
		//那么当目前仓位为0，或者 目前仓位和未完成数量方向相同
		//这样也要全部撤销
		//if (realPos == 0 || (realPos * undone > 0))
		if (decimal::eq(realPos, 0) || decimal::gt(realPos * undone, 0))
		{
			bool isBuy = decimal::gt(undone, 0);
			OrderIDs ids = _ctx->cancel(stdCode, isBuy);
			_orders_mon.push_order(ids.data(), ids.size(), _ctx->getCurTime());
			_cancel_cnt += ids.size();
			_ctx->writeLog("@ %d cancelcnt -> %u", __LINE__, _cancel_cnt);
			return;
		}
	}

	//如果都是同向的，则纳入计算
	double curPos = realPos + undone;
	//if (curPos == newVol)
	if (decimal::eq(curPos, newVol))
		return;

	if(_last_tick == NULL)
	{
		//grabLastTick会自动增加引用计数，不需要再retain
		_last_tick = _ctx->grabLastTick(stdCode);
	}

	if (_last_tick == NULL)
	{
		_ctx->writeLog("%s没有最新tick数据，退出执行逻辑", _code.c_str());
		return;
	}

	double buyPx, sellPx;
	if(_price_mode == -1)
	{
		buyPx = _last_tick->bidprice(0) + _comm_info->getPriceTick() * _price_offset;
		sellPx = _last_tick->askprice(0) - _comm_info->getPriceTick() * _price_offset;
	}
	else if(_price_mode == 0)
	{
		buyPx = _last_tick->price() + _comm_info->getPriceTick() * _price_offset;
		sellPx = _last_tick->price() - _comm_info->getPriceTick() * _price_offset;
	}
	else if(_price_mode == 1)
	{
		buyPx = _last_tick->askprice(0) + _comm_info->getPriceTick() * _price_offset;
		sellPx = _last_tick->bidprice(0) - _comm_info->getPriceTick() * _price_offset;
	}
	else if(_price_mode == 2)
	{
		double mp = (_last_tick->bidqty(0) - _last_tick->askqty(0))*1.0 / (_last_tick->bidqty(0) + _last_tick->askqty(0));
		bool isUp = (mp > 0);
		if(isUp)
		{
			buyPx = _last_tick->askprice(0) + _comm_info->getPriceTick() * _cancel_times;
			sellPx = _last_tick->askprice(0) - _comm_info->getPriceTick() * _cancel_times;
		}
		else
		{
			buyPx = _last_tick->bidprice(0) + _comm_info->getPriceTick() * _cancel_times;
			sellPx = _last_tick->bidprice(0) - _comm_info->getPriceTick() * _cancel_times;
		}
	}

	//if (newVol > curPos)
	if (decimal::gt(newVol, curPos))
	{
		OrderIDs ids = _ctx->buy(stdCode, buyPx, newVol - curPos);
		_orders_mon.push_order(ids.data(), ids.size(), _ctx->getCurTime());
	}
	else
	{
		OrderIDs ids  = _ctx->sell(stdCode, sellPx, curPos - newVol);
		_orders_mon.push_order(ids.data(), ids.size(), _ctx->getCurTime());
	}
}

void WtSimpExeUnit::set_position(const char* stdCode, double newVol)
{
	if (_code.compare(stdCode) != 0)
		return;

	if (_target_pos != newVol)
	{
		_target_pos = newVol;
	}

	doCalculate();
}
