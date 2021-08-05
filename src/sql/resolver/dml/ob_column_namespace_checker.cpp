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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_column_namespace_checker.h"
#include "lib/charset/ob_charset.h"
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/ob_resolver_define.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/resolver/ob_schema_checker.h"
#include "common/ob_smart_call.h"
namespace oceanbase {
using namespace common;
namespace sql {
const TableItem* ObColumnNamespaceChecker::ObTableItemIterator::get_next_table_item()
{
  const TableItem* table_item = NULL;
  if (table_container_.cur_joined_table_ != NULL) {
    // current_table_ is not null, means that we are resolving join table at present
    // so we can't touch the attribute of table in the current level
    if (0 == next_table_item_idx_) {
      ++next_table_item_idx_;
      table_item = table_container_.cur_joined_table_->left_table_;
    } else if (1 == next_table_item_idx_) {
      ++next_table_item_idx_;
      table_item = table_container_.cur_joined_table_->right_table_;
    } else {
      table_item = NULL;  // iterator end
    }
  } else {
    if (next_table_item_idx_ < table_container_.all_table_refs_.count()) {
      table_item = table_container_.all_table_refs_.at(next_table_item_idx_++);
    } else {
      table_item = NULL;
    }
  }
  return table_item;
}

int ObColumnNamespaceChecker::remove_reference_table(int64_t tid)
{
  int ret = OB_SUCCESS;
  cur_joined_table_ = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && i < all_table_refs_.count(); i++) {
    if (tid == all_table_refs_.at(i)->table_id_) {
      if (OB_FAIL(all_table_refs_.remove(i))) {
        LOG_WARN("failed to remove all_table_refs", K(ret));
      }
      break;
    }
  }
  return ret;
}

/*
 * if database name or table name is not specified, we must check the uniqueness of column in the table with the same
 * name for oracle mode, if table name is specified, we need to make sure that this column does not appear in the using
 * columns in the joined table for example, select t1.a from t1 inner join t2 using (a), this is not allowed in oracle
 * mode
 */
int ObColumnNamespaceChecker::check_table_column_namespace(const ObQualifiedName& q_name, const TableItem*& table_item)
{
  int ret = OB_SUCCESS;
  table_item = NULL;
  const TableItem* cur_table = NULL;
  bool need_check_unique = false;
  // Special testing is required for multi table insert, because the same table may
  // be inserted multiple times in the same sql.
  // insert all into t1 values(1,1) into t1 values(2,2) select 1 from dual;
  if (get_resolve_params().is_multi_table_insert_) {
    if (OB_ISNULL(cur_table = all_table_refs_.at(all_table_refs_.count() - 1))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected error", K(cur_table), K(ret));
    } else if (OB_FAIL(find_column_in_table(*cur_table, q_name, table_item, need_check_unique))) {
      LOG_WARN("failed to find column in table ", K(ret));
    } else { /*do nothing*/
    }
  } else {
    ObTableItemIterator table_item_iter(*this);
    while (OB_SUCC(ret) && (cur_table = table_item_iter.get_next_table_item()) != NULL) {
      if (OB_FAIL(find_column_in_table(*cur_table, q_name, table_item, need_check_unique))) {
        if (OB_ERR_BAD_FIELD_ERROR == ret) {
          ret = OB_SUCCESS;
          // continue to search
        } else {
          LOG_WARN("find column in table failed", K(ret));
        }
      } else {
        break;  // found column in table
      }
    }
    if (OB_SUCC(ret) && NULL == table_item) {
      ret = OB_ERR_BAD_FIELD_ERROR;
    }
    if (OB_SUCC(ret) && need_check_unique) {
      // check table column whether unique in all tables
      const TableItem* tmp_table = NULL;
      bool tmp_check = false;
      while (OB_SUCC(ret) && (cur_table = table_item_iter.get_next_table_item()) != NULL) {
        if (OB_FAIL(find_column_in_table(*cur_table, q_name, tmp_table, tmp_check))) {
          if (OB_ERR_BAD_FIELD_ERROR == ret) {
            ret = OB_SUCCESS;
            // continue to search
          }
        } else if (table_item->table_id_ != tmp_table->table_id_) {
          ret = OB_NON_UNIQ_ERROR;
          LOG_WARN("column in all tables is ambiguous", K(q_name));
        }
      }
    }
    if (OB_SUCC(ret) && table_item != NULL && !q_name.tbl_name_.empty()) {
      ret = check_column_existence_in_using_clause(table_item->table_id_, q_name.col_name_);
    }
  }
  return ret;
}

int ObColumnNamespaceChecker::check_column_existence_in_using_clause(
    const uint64_t table_id, const common::ObString& column_name)
{
  int ret = OB_SUCCESS;
  const TableItem* cur_table = NULL;
  ObTableItemIterator table_item_iter(*this);
  while (OB_SUCC(ret) && (cur_table = table_item_iter.get_next_table_item()) != NULL) {
    bool exist = false;
    if (OB_FAIL(check_column_existence_in_using_clause(table_id, column_name, *cur_table, exist))) {
      LOG_WARN("failed to check column existence in using clause", K(ret));
    } else if (exist) {
      ret = OB_ERR_QUALIFIER_EXISTS_FOR_USING_COLUMN;
      LOG_WARN("column part of using clause can not have qualifier", K(ret));
    } else { /*do nothing*/
    }
  }
  return ret;
}

int ObColumnNamespaceChecker::check_column_existence_in_using_clause(
    const uint64_t table_id, const common::ObString& column_name, const TableItem& table_item, bool& exist)
{
  int ret = OB_SUCCESS;
  if (!table_item.is_joined_table()) {
    /*do nothing*/
  } else {
    const JoinedTable& joined_table = static_cast<const JoinedTable&>(table_item);
    bool table_exist = false;
    if (share::is_oracle_mode() || FULL_OUTER_JOIN == joined_table.joined_type_) {
      for (int64_t i = 0; OB_SUCC(ret) && !table_exist && i < joined_table.single_table_ids_.count(); i++) {
        if (joined_table.single_table_ids_.at(i) == table_id) {
          table_exist = true;
        } else { /*do nothing*/
        }
      }
      if (OB_SUCC(ret) && table_exist) {
        for (int64_t i = 0; OB_SUCC(ret) && !exist && i < joined_table.using_columns_.count(); i++) {
          if (0 == column_name.case_compare(joined_table.using_columns_.at(i))) {
            exist = true;
          }
        }
      }
    }
    if (OB_SUCC(ret) && table_exist && !exist) {
      if (OB_ISNULL(joined_table.left_table_) || OB_ISNULL(joined_table.right_table_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(joined_table.left_table_), K(joined_table.right_table_), K(ret));
      } else if (OB_FAIL(SMART_CALL(check_column_existence_in_using_clause(
                     table_id, column_name, *joined_table.left_table_, exist)))) {
        LOG_WARN("failed to check column existence in using scope", K(ret));
      } else if (exist) {
        /*do nothing*/
      } else if (OB_FAIL(SMART_CALL(check_column_existence_in_using_clause(
                     table_id, column_name, *joined_table.right_table_, exist)))) {
        LOG_WARN("failed to check column existence in using scope", K(ret));
      } else { /*do nothing*/
      }
    }
  }
  return ret;
}

int ObColumnNamespaceChecker::check_using_column_namespace(
    const ObString& column_name, const TableItem*& left_table, const TableItem*& right_table)
{
  int ret = OB_SUCCESS;
  const TableItem* l_table = NULL;
  const TableItem* r_table = NULL;
  ObQualifiedName q_name;
  q_name.col_name_ = column_name;
  bool l_need_check_unique = false;
  bool r_need_check_unique = false;
  // using column search order:
  // 1. right table
  // 2. left table
  if (OB_ISNULL(cur_joined_table_) || OB_ISNULL(l_table = cur_joined_table_->left_table_) ||
      OB_ISNULL(r_table = cur_joined_table_->right_table_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("current joined table is null", K_(cur_joined_table), K(l_table), K(r_table));
  } else if (OB_FAIL(find_column_in_table(*r_table, q_name, right_table, r_need_check_unique))) {
    LOG_WARN("find column in right joined table failed", K(ret), K(column_name));
  } else if (OB_FAIL(find_column_in_table(*l_table, q_name, left_table, l_need_check_unique))) {
    LOG_WARN("find column in left joined table failed", K(ret), K(column_name));
  }
  return ret;
}

int ObColumnNamespaceChecker::check_column_exists(const TableItem& table_item, const ObString& col_name, bool& is_exist)
{
  int ret = OB_SUCCESS;
  is_exist = false;
  // for materialized view, should use materialized view id to resolve column,
  // and its schema id saved in table_item.table_id_
  uint64_t table_id = table_item.is_materialized_view_ ? table_item.table_id_ : table_item.ref_id_;
  if (OB_ISNULL(params_.schema_checker_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("schema_checker is null");
  } else if (table_item.is_basic_table()) {
    // check column name in schema checker
    if (OB_FAIL(params_.schema_checker_->check_column_exists(table_id, col_name, is_exist))) {
      LOG_WARN("check column exists failed", K(ret));
    }
  } else if (table_item.is_generated_table()) {
    ObSelectStmt* ref_stmt = table_item.ref_query_;
    if (OB_ISNULL(ref_stmt)) {
      ret = OB_NOT_INIT;
      LOG_WARN("generate table ref stmt is null");
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < ref_stmt->get_select_item_size(); ++i) {
      if (ObCharset::case_compat_mode_equal(col_name, ref_stmt->get_select_item(i).alias_name_)) {
        if (!is_exist) {
          is_exist = true;
          if (!share::is_oracle_mode()) {
            break;
          }
        } else {
          ret = OB_NON_UNIQ_ERROR;
          LOG_WARN(
              "column duplicated, should happen in ORACLE mode only", K(share::is_oracle_mode()), K(col_name), K(ret));
          ObString scope_name = ObString::make_string(get_scope_name(T_FIELD_LIST_SCOPE));
          LOG_USER_ERROR(OB_NON_UNIQ_ERROR, col_name.length(), col_name.ptr(), scope_name.length(), scope_name.ptr());
        }
      }
    }
  } else if (table_item.is_fake_cte_table()) {
    if (OB_FAIL(params_.schema_checker_->check_column_exists(table_id, col_name, is_exist))) {
      LOG_WARN("check column exists failed", K(ret));
    }
  } else if (table_item.is_function_table()) {
    if (OB_FAIL(ObResolverUtils::check_function_table_column_exist(table_item, params_, col_name))) {
      LOG_WARN("failed to check function table column exist", K(ret), K(col_name));
    } else {
      is_exist = true;
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected table type", K_(table_item.type));
  }
  return ret;
}

int ObColumnNamespaceChecker::set_equal_columns(const common::ObIArray<ObString>& columns)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(equal_columns_.assign(columns))) {
    LOG_WARN("failed to assign equal columns", K(ret));
  }
  return ret;
}

void ObColumnNamespaceChecker::clear_equal_columns()
{
  equal_columns_.reset();
}

int ObColumnNamespaceChecker::find_column_in_single_table(
    const TableItem& table_item, const ObQualifiedName& q_name, bool& need_check_unique)
{
  int ret = OB_SUCCESS;
  need_check_unique = check_unique_ && (q_name.database_name_.empty() || q_name.tbl_name_.empty());
  if (OB_UNLIKELY(!equal_columns_.empty())) {
    for (int64_t i = 0; OB_SUCC(ret) && need_check_unique && i < equal_columns_.count(); ++i) {
      if (0 == q_name.col_name_.case_compare(equal_columns_.at(i))) {
        need_check_unique = false;
      }
    }
  }
  // if databasename or table name is not specified,
  // we must check the uniqueness of column in the table with the same name
  bool is_match = true;
  LOG_TRACE("column info", K(q_name), K(table_item));
  if (!q_name.database_name_.empty()) {
    if (OB_FAIL(ObResolverUtils::name_case_cmp(
            params_.session_info_, q_name.database_name_, table_item.database_name_, OB_TABLE_NAME_CLASS, is_match))) {
      LOG_WARN("database name case compare failed", K(ret));
    }
    if (OB_SUCC(ret) && !is_match && !table_item.synonym_db_name_.empty() &&
        OB_FAIL(ObResolverUtils::name_case_cmp(params_.session_info_,
            q_name.database_name_,
            table_item.synonym_db_name_,
            OB_TABLE_NAME_CLASS,
            is_match))) {
      LOG_WARN("database name case compare failed", K(ret));
    }
  }
  if (OB_SUCC(ret) && is_match && !q_name.tbl_name_.empty()) {
    if (OB_FAIL(ObResolverUtils::name_case_cmp(
            params_.session_info_, q_name.tbl_name_, table_item.get_object_name(), OB_TABLE_NAME_CLASS, is_match))) {
      LOG_WARN("database name case compare failed", K(ret));
    }
  }
  if (OB_SUCC(ret) && is_match) {
    if (OB_FAIL(check_column_exists(table_item, q_name.col_name_, is_match))) {
      LOG_WARN("check column exists failed", K(ret));
    }
  }
  if (OB_SUCC(ret) && !is_match) {
    ret = OB_ERR_BAD_FIELD_ERROR;
  }
  return ret;
}

// check column in joined tables, searching from root_table,
// which is the searching scope passed by caller.
// found_item is fill with the first matched table, and the
// table is determined by join type.
int ObColumnNamespaceChecker::find_column_in_joined_table(const JoinedTable& joined_table,
    const ObQualifiedName& q_name, const TableItem*& found_table, bool& need_check_unique)
{
  int ret = OB_SUCCESS;
  if (hit_join_table_using_name(joined_table, q_name)) {
    if (FULL_OUTER_JOIN == joined_table.joined_type_) {
      found_table = &joined_table;
      need_check_unique = true;
    } else if (RIGHT_OUTER_JOIN == joined_table.joined_type_) {
      ret = find_column_in_table(*joined_table.right_table_, q_name, found_table, need_check_unique);
    } else {
      ret = find_column_in_table(*joined_table.left_table_, q_name, found_table, need_check_unique);
    }
  } else {
    // search order
    // left joined table
    // right joined table
    if (OB_FAIL(find_column_in_table(*joined_table.left_table_, q_name, found_table, need_check_unique))) {
      if (OB_ERR_BAD_FIELD_ERROR == ret) {
        ret = find_column_in_table(*joined_table.right_table_, q_name, found_table, need_check_unique);
      } else {
        LOG_WARN("find column in left table failed", K(ret), K(q_name));
      }
    } else if (need_check_unique) {
      // check table column whether unique in joined table
      const TableItem* tmp_found_table = NULL;
      bool tmp_need_check = false;
      if (OB_FAIL(find_column_in_table(*joined_table.right_table_, q_name, tmp_found_table, tmp_need_check))) {
        if (OB_ERR_BAD_FIELD_ERROR == ret) {
          ret = OB_SUCCESS;
        } else {
          LOG_WARN("find column in right table failed", K(ret));
        }
      } else {
        ret = OB_NON_UNIQ_ERROR;
        LOG_WARN("column in joined table is ambiguous", K(q_name));
      }
    }
  }
  return ret;
}

int ObColumnNamespaceChecker::find_column_in_table(
    const TableItem& table_item, const ObQualifiedName& q_name, const TableItem*& found_table, bool& need_check_unique)
{
  int ret = OB_SUCCESS;
  if (table_item.is_joined_table()) {
    const JoinedTable& joined_table = static_cast<const JoinedTable&>(table_item);
    ret = find_column_in_joined_table(joined_table, q_name, found_table, need_check_unique);
  } else {
    if (OB_SUCC(find_column_in_single_table(table_item, q_name, need_check_unique))) {
      found_table = &table_item;
    }
  }
  return ret;
}

bool ObColumnNamespaceChecker::hit_join_table_using_name(const JoinedTable& joined_table, const ObQualifiedName& q_name)
{
  bool bret = false;
  if (q_name.tbl_name_.empty()) {
    for (int64_t i = 0; i < joined_table.using_columns_.count(); ++i) {
      if (ObCharset::case_insensitive_equal(q_name.col_name_, joined_table.using_columns_.at(i))) {
        bret = true;
        break;
      }
    }
  }
  return bret;
}

int ObColumnNamespaceChecker::check_rowscn_table_column_namespace(
    const ObQualifiedName& q_name, const TableItem*& table_item)
{
  int ret = OB_SUCCESS;
  table_item = nullptr;
  const TableItem* cur_table = nullptr;
  ObTableItemIterator table_item_iter(*this);
  while (OB_SUCC(ret) && (cur_table = table_item_iter.get_next_table_item()) != nullptr) {
    if (!cur_table->is_basic_table()) {
    } else if (q_name.tbl_name_.empty()) {
      if (NULL == table_item) {
        table_item = cur_table;
      } else {
        ret = OB_NON_UNIQ_ERROR;
        LOG_WARN("column in all tables is ambiguous", K(ret), K(q_name));
      }
    } else {
      bool is_match = true;
      if (OB_FAIL(ObResolverUtils::name_case_cmp(
              params_.session_info_, q_name.tbl_name_, cur_table->get_object_name(), OB_TABLE_NAME_CLASS, is_match))) {
        LOG_WARN("table name case compare failed", K(ret));
      } else if (is_match) {
        table_item = cur_table;
        break;
      }
    }
  }

  return ret;
}

}  // namespace sql
}  // namespace oceanbase
