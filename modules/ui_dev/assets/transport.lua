-- The Transport panel's model. ui_dev writes it from player.transport; the panel's presses
-- go out as events (play, pause) that ui_dev turns into the player's commands.
return {
  model = { t_state = "", t_pos = "0:00", t_len = "0:00" },
  on = {},
}
