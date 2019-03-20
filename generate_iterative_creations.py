import os


organisms = ["Ailuropoda_melanoleuca","Ananas_comosus","Apis_florea","Asparagus_officinalis","Cajanus_cajan","Camelus_bactrianus","Cariama_cristata","Chaetura_pelagica","Colius_striatus","Columba_livia","Corvus_cornix","Drosophila_eugracilis","Drosophila_melanogaster","Egretta_garzetta","Equus_przewalskii","Falco_cherrug","Falco_peregrinus","Latimeria_chalumnae","Maylandia_zebra","Melopsittacus_undulatus","Microcebus_murinus","Oncorhynchus_kisutch","Otolemur_garnettii","Parasteatoda_tepidariorum","Pelodiscus_sinensis","Picoides_pubescens","Prunus_persica","Solanum_tuberosum","Tupaia_chinensis","Tyto_alba"]

# script to clean MySQL database
scripts_path = "/home/i/Documents/introns-db-fill"
increment_template_script = os.path.join(scripts_path, "iterative_create_database.sql")
template = open(increment_template_script, "r").read()
templates_path = "/home/i/Documents/introns-db-fill/iterative_creations"

for idx, org in enumerate(organisms):
	print("=========================================")

	print("{0} =============".format(org))
	org_template = template.replace("%", org)
	org_template = org_template.replace("?", str((idx+1)*5000000))
	org_template_path = os.path.join(templates_path, org+".sql")
	with open(org_template_path, "w") as org_template_file:
		org_template_file.write(org_template)
	print("=========================================\n\n")

