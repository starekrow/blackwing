-- The Library panel's model: the roots; which view, sort and search; where the list is
-- (l_crumb); the rows in view (l_rows, the first of them row l_first of l_total); and a
-- track being edited.
return {
  model = {
    l_status = "", l_roots = {}, l_root_path = "",
    l_view = "folders", l_sort = "default", l_search = "", l_crumb = "",
    l_rows = {}, l_first = 0, l_total = 0,
    l_edit_open = false, l_edit_name = "", l_edit_field = "title", l_edit_value = "",
  },
  on = {},
}
