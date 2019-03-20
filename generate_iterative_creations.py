import os


organisms = ["Ailuropoda_melanoleuca","Ananas_comosus","Apis_florea","Asparagus_officinalis","Cajanus_cajan","Camelus_bactrianus","Cariama_cristata","Chaetura_pelagica","Colius_striatus","Columba_livia","Corvus_cornix","Drosophila_eugracilis","Drosophila_melanogaster","Egretta_garzetta","Equus_przewalskii","Falco_cherrug","Falco_peregrinus","Latimeria_chalumnae","Maylandia_zebra","Melopsittacus_undulatus","Microcebus_murinus","Oncorhynchus_kisutch","Otolemur_garnettii","Parasteatoda_tepidariorum","Pelodiscus_sinensis","Picoides_pubescens","Prunus_persica","Solanum_tuberosum","Tupaia_chinensis","Tyto_alba"]

# script to clean MySQL database
scripts_path = "/home/i/Documents/introns-db-fill"
increment_template_script = os.path.join(scripts_path, "iterative_create_database.sql")
template = open(increment_template_script, "r").read()
templates_path = os.path.join(scripts_path, "iterative_creations")


start_script = os.path.join(templates_path, "start.sh")
create_databases_script = os.path.join(templates_path, "create_databases.sql")
drop_databases_script = os.path.join(templates_path, "drop_databases.sql")

start = ""
create_database_template = ""
drop_databases_template = ""

for idx, org in enumerate(organisms):
	# print("=========================================")

	print("{0} =============".format(org))
	org_template = template.replace("%", org)
	org_template = org_template.replace("?", str((idx+1)*5000000))
	org_template_path = os.path.join(templates_path, org+".sql")
	with open(org_template_path, "w") as org_template_file:
		org_template_file.write(org_template)
	# print("=========================================\n\n")

	
	start_template = "mysql --user=root --database="+org+" --password=1"
	start_template = "{0} < {1}\n".format(start_template, org_template_path)	
	start += start_template

	create_database_template += "CREATE DATABASE " + org + ";\n"
	drop_databases_template += "DROP DATABASE IF EXISTS " + org + ";\n"

with open(start_script, "w") as start_script_file:
		start_script_file.write(start)

with open(create_databases_script, "w") as create_databases_script_file:
		create_databases_script_file.write(create_database_template)

with open(drop_databases_script, "w") as drop_databases_script_file:
		drop_databases_script_file.write(drop_databases_template)