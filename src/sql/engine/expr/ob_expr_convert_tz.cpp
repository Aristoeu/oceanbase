/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_convert_tz.h"
#include "lib/timezone/ob_time_convert.h"
#include "lib/timezone/ob_timezone_info.h"
#include "lib/ob_name_def.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_exec_context.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

ObExprConvertTZ::ObExprConvertTZ(common::ObIAllocator &alloc): 
ObFuncExprOperator(alloc, T_FUN_SYS_CONVERT_TZ, "convert_TZ", 3, NOT_ROW_DIMENSION){}

int ObExprConvertTZ::cg_expr(ObExprCGCtx &expr_cg_ctx, const ObRawExpr &raw_expr,
                                  ObExpr &expr) const
{
  int ret = OB_SUCCESS;
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  CK(3 == expr.arg_cnt_ );
  CK(OB_NOT_NULL(expr.args_) && OB_NOT_NULL(expr.args_[0]) 
    && OB_NOT_NULL(expr.args_[1]) && OB_NOT_NULL(expr.args_[2]));
  CK(ObDateTimeType == expr.args_[0]->datum_meta_.type_);
  CK(ObDateTimeType == expr.datum_meta_.type_);
  CK(ObVarcharType == expr.args_[1]->datum_meta_.type_);
  CK(ObVarcharType == expr.args_[2]->datum_meta_.type_);
  OX(expr.eval_func_ = ObExprConvertTZ::eval_convert_tz);
  return ret;
}  

int ObExprConvertTZ::eval_convert_tz(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res){
  int ret = OB_SUCCESS;
  ObDatum *timestamp = NULL;
  ObDatum *time_zone_s = NULL;
  ObDatum *time_zone_d = NULL;
  if (OB_FAIL(expr.eval_param_value(ctx, timestamp, time_zone_s, time_zone_d))) {
    LOG_WARN("calc param value failed", K(ret));
  } else if (OB_UNLIKELY(timestamp->is_null() || time_zone_s->is_null() || time_zone_d->is_null())) {
    res.set_null();
  } else {
    int64_t timestamp_data = timestamp->get_datetime();
    if (OB_FAIL(calc_convert_tz(timestamp_data, time_zone_s->get_string(), time_zone_d->get_string(), 
                                  ctx.exec_ctx_.get_my_session()))) {
      LOG_WARN("calc convert tz zone failed", K(ret));
    } else {
      res.set_datetime(timestamp_data);
    }
  }
  return ret;
}

int ObExprConvertTZ::calc_result_type3(ObExprResType &type,
                                        ObExprResType &input1,
                                        ObExprResType &input2,
                                        ObExprResType &input3,
                                        common::ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;
  const ObSQLSessionInfo *session = NULL;
  if (OB_ISNULL(session = type_ctx.get_session())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session is null", K(ret));
  } else {
    type.set_datetime();
    input1.set_calc_type(ObDateTimeType);
    input2.set_calc_type(ObVarcharType);
    input3.set_calc_type(ObVarcharType);
    input2.set_calc_collation_type(session->get_nls_collation());
    input3.set_calc_collation_type(session->get_nls_collation());
  }
  return ret;
}

int ObExprConvertTZ::calc_result3(common::ObObj &result,
                                  const common::ObObj &input1,
                                  const common::ObObj &input2,
                                  const common::ObObj &input3,
                                  common::ObExprCtx &expr_ctx) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(input1.is_null() || input2.is_null() || input3.is_null())) {
    result.set_null();
  } else {
    int64_t timestamp_data = input1.get_datetime();
    if (OB_FAIL(calc_convert_tz(timestamp_data, input2.get_string(), input3.get_string(), 
                                  expr_ctx.my_session_))) {
      LOG_WARN("convert time zone failed", K(ret));
    } else {
      result.set_datetime(timestamp_data);
    }
  }
  return ret;
}                                  


int ObExprConvertTZ::find_time_zone_pos(const ObString &tz_name,
                                        const ObTimeZoneInfo &tz_info,
                                        ObTimeZoneInfoPos *&tz_info_pos)
{
  int ret = OB_SUCCESS;
  tz_info_pos = NULL;
  ObTZInfoMap *tz_info_map = NULL;
  if (OB_ISNULL(tz_info_map = const_cast<ObTZInfoMap *>(tz_info.get_tz_info_map()))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tz_info_map is NULL", K(ret));
  } else if (OB_FAIL(tz_info_map->get_tz_info_by_name(tz_name, tz_info_pos))) {
    LOG_WARN("fail to get_tz_info_by_name", K(tz_name), K(ret));
    tz_info_map->id_map_.revert(tz_info_pos);
    tz_info_pos = NULL;
  } else {
    tz_info_pos->set_error_on_overlap_time(tz_info.is_error_on_overlap_time());
    // need to test overlap time
  }
  return ret;
}

int ObExprConvertTZ::parse_string(int64_t &timestamp_data, const ObString &tz_str, ObSQLSessionInfo *session, int32_t &offset)
{
	int ret = OB_SUCCESS;
	int ret_more = 0;
	if (OB_FAIL(ObTimeConverter::str_to_offset(tz_str, offset, ret_more,
                              false /* oracle_mode */, true /* need_check_valid */)))
	{
		LOG_WARN("direct str_to_offset failed");
		if (OB_LIKELY(OB_ERR_UNKNOWN_TIME_ZONE == ret)){
			const ObTimeZoneInfo *tz_info = NULL;
      ObTimeZoneInfoPos *target_tz_pos = NULL;
      if (OB_ISNULL(tz_info = TZ_INFO(session))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("tz info is null", K(ret), K(session));
      } else if (OB_FAIL(find_time_zone_pos(tz_str, *tz_info, target_tz_pos))){
        LOG_WARN("find time zone position failed", K(ret), K(ret_more));
        if (OB_ERR_UNKNOWN_TIME_ZONE == ret && OB_SUCCESS != ret_more) {
          ret = ret_more;
        }
      } else if (OB_FAIL(calc(timestamp_data, *target_tz_pos, offset))) {
        LOG_WARN("calc failed", K(ret), K(timestamp_data));
      } 
		} else {
      LOG_WARN("str to offset failed", K(ret));
    }
	} else {
			LOG_DEBUG("str to offset succeed", K(tz_str), K(offset));
	}
	return ret;
}

int ObExprConvertTZ::calc_convert_tz(int64_t &timestamp_data,
                                    const ObString &tz_str_s,//source time zone (input2)
                                    const ObString &tz_str_d,//destination time zone (input3)
                                    ObSQLSessionInfo *session)
{
	int ret = OB_SUCCESS;
	int32_t offset_s = 0;
  int32_t offset_d = 0;
	if(OB_FAIL(ObExprConvertTZ::parse_string(timestamp_data, tz_str_s,session, offset_s))){
		LOG_WARN("source time zone parse failed", K(tz_str_s));
	} else if(OB_FAIL(ObExprConvertTZ::parse_string(timestamp_data, tz_str_d,session, offset_d))){
		LOG_WARN("source time zone parse failed", K(tz_str_d));
	} else {
		timestamp_data = timestamp_data + (static_cast<int64_t>(offset_d - offset_s)) * 1000000;
	}
	return ret;
}																		

int ObExprConvertTZ::calc(int64_t &timestamp_data, const ObTimeZoneInfoPos &tz_info_pos, int32_t &offset_sec)
{
  int ret = OB_SUCCESS;
  offset_sec = 0;
  int32_t tz_id = tz_info_pos.get_tz_id();
  int32_t tran_type_id = 0;
  ObString tz_abbr_str;
  if (OB_FAIL(tz_info_pos.get_timezone_offset(timestamp_data, offset_sec,
                                              tz_abbr_str,tran_type_id))) {//the result value is in offset_sec
    LOG_WARN("get timezone sub offset failed", K(ret));
  } else {
    LOG_DEBUG("at time zone base calc", K(timestamp_data), K(tz_id), K(tran_type_id));
  }
  return ret;
}


}
}