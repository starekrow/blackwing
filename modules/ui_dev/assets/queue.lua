-- The Queue panel's model: the rows in view (q_rows, the first of them row q_first of
-- q_total), a line of counts, and the path being typed.
return {
  model = { q_summary = "", q_note = "", q_path = "", q_rows = {}, q_first = 0, q_total = 0 },
  on = {},
}
