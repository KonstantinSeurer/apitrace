
import sys

file_path = sys.argv[1]
variable_name = sys.argv[2]

with open(file_path, 'r') as file:
    file_content = file.read()

    print(f'static const char *{variable_name} = R"(')
    print(file_content)
    print(f')";')
