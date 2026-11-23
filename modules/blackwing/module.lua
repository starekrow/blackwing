-- The executable: the module list, the command line, which face is up. For now it says
-- so and exits; the first slice makes it play (docs/PRDs/Blackwing-Master-PRD.md).
module "blackwing" {
  output = "exe",
  deps   = { "core" },
}
