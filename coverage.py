#!/usr/bin/env python3
"""
Simple C++ Code Coverage Instrumenter using regex
Inserts COVER() calls after opening braces in functions, if/else blocks, and switch cases.
"""

import sys
import re
import argparse
import os

class SimpleCoverageInstrumenter:
    def __init__(self, source_code, debug=False):
        self.source_code = source_code
        self.lines = source_code.split('\n')
        self.counter = 0
        self.coverage_points = []
        self.debug = debug

    def get_next_id(self, prefix="point"):
        self.counter += 1
        return f"{prefix}_{self.counter}"

    def find_opening_brace(self, start_line):
        """Look for opening brace starting from start_line"""
        for i in range(start_line, min(start_line + 10, len(self.lines))):
            if '{' in self.lines[i]:
                return i
        return None

    def instrument(self):
        """Instrument the C++ code using simple regex patterns"""
        instrumented_lines = []
        i = 0
        skip_until = -1  # Skip lines until this index when we've already processed them
        in_nocover_section = False  # Track if we're in a NOCOVER section

        while i < len(self.lines):
            line = self.lines[i]

            # Check for NOCOVER markers
            if '/*NOCOVER_START*/' in line:
                in_nocover_section = True
                instrumented_lines.append(line)
                i += 1
                continue

            if '/*NOCOVER_END*/' in line:
                in_nocover_section = False
                instrumented_lines.append(line)
                i += 1
                continue

            # Skip instrumentation if in NOCOVER section
            if in_nocover_section:
                instrumented_lines.append(line)
                i += 1
                continue

            if i < skip_until:
                instrumented_lines.append(self.lines[i])
                i += 1
                continue

            # Skip coverage header sections
            if '===== COVERAGE TRACKING CODE =====' in line:
                while i < len(self.lines) and '===== END COVERAGE TRACKING =====' not in self.lines[i]:
                    instrumented_lines.append(self.lines[i])
                    i += 1
                if i < len(self.lines):
                    instrumented_lines.append(self.lines[i])
                i += 1
                continue

            # Skip existing COVER calls
            if 'COVER(' in line:
                instrumented_lines.append(line)
                i += 1
                continue

            # Function definitions - look for function-like pattern
            func_match = re.match(r'^[^/]*\b\w+\s+(\w+)\s*\([^)]*\)', line)
            if func_match and not re.match(r'^\s*(if|while|for|switch)\s*\(', line):
                func_name = func_match.group(1)
                # Find the opening brace
                brace_line = self.find_opening_brace(i)
                if brace_line is not None:
                    # Add all lines up to and including the brace
                    for j in range(i, brace_line + 1):
                        instrumented_lines.append(self.lines[j])

                    # Add coverage point
                    cover_id = f"{func_name}_entry"
                    self.coverage_points.append(cover_id)

                    # Determine indentation for next line
                    indent = '    '
                    if brace_line + 1 < len(self.lines):
                        next_line = self.lines[brace_line + 1]
                        if next_line.strip():
                            indent = ' ' * (len(next_line) - len(next_line.lstrip()))

                    instrumented_lines.append(f'{indent}COVER("{cover_id}");')
                    skip_until = brace_line + 1
                    i = brace_line + 1
                    continue

            # Look for else if
            elif_match = re.search(r'\belse\s+if\s*\(', line)
            if elif_match:
                # Find the opening brace
                brace_line = self.find_opening_brace(i)
                if brace_line is not None:
                    # Add all lines up to and including the brace
                    for j in range(i, brace_line + 1):
                        instrumented_lines.append(self.lines[j])

                    # Add coverage point
                    cover_id = self.get_next_id("else")
                    self.coverage_points.append(cover_id)

                    # Determine indentation
                    indent = '    '
                    if brace_line + 1 < len(self.lines):
                        next_line = self.lines[brace_line + 1]
                        if next_line.strip():
                            indent = ' ' * (len(next_line) - len(next_line.lstrip()))

                    instrumented_lines.append(f'{indent}COVER("{cover_id}");')
                    skip_until = brace_line + 1
                    i = brace_line + 1
                    continue

            # Look for plain else (but not else if)
            else_match = re.search(r'\belse\b(?!\s+if)', line)
            if else_match:
                # Find the opening brace
                brace_line = self.find_opening_brace(i)
                if brace_line is not None:
                    # Add all lines up to and including the brace
                    for j in range(i, brace_line + 1):
                        instrumented_lines.append(self.lines[j])

                    # Add coverage point
                    cover_id = self.get_next_id("else")
                    self.coverage_points.append(cover_id)

                    # Determine indentation
                    indent = '    '
                    if brace_line + 1 < len(self.lines):
                        next_line = self.lines[brace_line + 1]
                        if next_line.strip():
                            indent = ' ' * (len(next_line) - len(next_line.lstrip()))

                    instrumented_lines.append(f'{indent}COVER("{cover_id}");')
                    skip_until = brace_line + 1
                    i = brace_line + 1
                    continue

            # Look for if (but not else if)
            if_match = re.search(r'(?<!\belse\s)\bif\s*\(', line)
            if if_match:
                # Find the opening brace
                brace_line = self.find_opening_brace(i)
                if brace_line is not None:
                    # Add all lines up to and including the brace
                    for j in range(i, brace_line + 1):
                        instrumented_lines.append(self.lines[j])

                    # Add coverage point
                    cover_id = self.get_next_id("if")
                    self.coverage_points.append(cover_id)

                    # Determine indentation
                    indent = '    '
                    if brace_line + 1 < len(self.lines):
                        next_line = self.lines[brace_line + 1]
                        if next_line.strip():
                            indent = ' ' * (len(next_line) - len(next_line.lstrip()))

                    instrumented_lines.append(f'{indent}COVER("{cover_id}");')
                    skip_until = brace_line + 1
                    i = brace_line + 1
                    continue

            # Switch case statements
            case_match = re.match(r'^(\s*)case\s+([^:]+):', line)
            if case_match:
                indent = case_match.group(1)
                case_value = case_match.group(2).strip()
                # Sanitize case value for ID
                case_value = re.sub(r'[^a-zA-Z0-9_]', '_', case_value)
                cover_id = self.get_next_id(f"case_{case_value}")
                self.coverage_points.append(cover_id)
                instrumented_lines.append(line)

                # Check if there's a statement on the same line after the colon
                if line.split(':', 1)[1].strip() and not line.split(':', 1)[1].strip().startswith('{'):
                    # Statement on same line, insert before it
                    parts = line.split(':', 1)
                    instrumented_lines[-1] = f"{parts[0]}: COVER(\"{cover_id}\"); {parts[1].strip()}"
                else:
                    # Add on next line
                    instrumented_lines.append(f'{indent}    COVER("{cover_id}");')
                i += 1
                continue

            # Default case
            default_match = re.match(r'^(\s*)default\s*:', line)
            if default_match:
                indent = default_match.group(1)
                cover_id = self.get_next_id("default")
                self.coverage_points.append(cover_id)
                instrumented_lines.append(line)
                instrumented_lines.append(f'{indent}    COVER("{cover_id}");')
                i += 1
                continue

            # No match, keep the line as is
            instrumented_lines.append(line)
            i += 1

        return '\n'.join(instrumented_lines)

    def generate_coverage_header(self):
        """Generate the coverage tracking code to be included"""
        header = """// ===== COVERAGE TRACKING CODE =====
#include <iostream>
#include <unordered_set>
#include <string>
#include <algorithm>
#include <vector>

// Global set of uncovered points - starts with all points
std::unordered_set<std::string> __uncovered_points = {"""


        sorted_points = sorted(self.coverage_points)

        # Add all coverage points
        points_list = ', '.join([f'"{p}"' for p in sorted_points])
        header += points_list

        header += """};

// Total number of coverage points
const size_t __total_points = """ + str(len(self.coverage_points)) + """;

// Function to mark coverage - removes from uncovered set
void COVER(const std::string& point) {
    __uncovered_points.erase(point);
}

// Function to print coverage report
void print_coverage_report() {
    size_t covered_count = __total_points - __uncovered_points.size();

    std::cout << "\\n===== COVERAGE REPORT =====\\n";
    std::cout << "Total coverage points: " << __total_points << "\\n";
    std::cout << "Points covered: " << covered_count << "\\n";
    std::cout << "Coverage: " << (100.0 * covered_count / __total_points) << "%\\n\\n";

    if (__uncovered_points.empty()) {
        std::cout << "✓ All paths covered!\\n";
    } else {
        std::cout << "Uncovered points:\\n";
        // Sort uncovered points for display
        std::vector<std::string> uncovered_sorted(__uncovered_points.begin(), __uncovered_points.end());
        std::sort(uncovered_sorted.begin(), uncovered_sorted.end());
        for (const auto& point : uncovered_sorted) {
            std::cout << "  ✗ " << point << "\\n";
        }
    }
}

// Automatically print report at program exit
struct CoverageReporter {
    ~CoverageReporter() {
        print_coverage_report();
    }
};
CoverageReporter __coverage_reporter;

// ===== END COVERAGE TRACKING =====

"""
        return header


def remove_coverage(source_code):
    """Remove coverage instrumentation from C++ code"""
    lines = source_code.split('\n')
    cleaned_lines = []
    in_coverage_header = False

    for line in lines:
        # Check for coverage header start
        if '===== COVERAGE TRACKING CODE =====' in line:
            in_coverage_header = True
            continue

        # Check for coverage header end
        if '===== END COVERAGE TRACKING =====' in line:
            in_coverage_header = False
            continue

        # Skip lines in coverage header
        if in_coverage_header:
            continue

        # Remove COVER() calls
        if 'COVER(' in line:
            # Check if it's a standalone COVER line
            if re.match(r'^\s*COVER\([^)]+\);\s*$', line):
                continue  # Skip the entire line
            else:
                # Remove inline COVER calls
                line = re.sub(r'\s*COVER\([^)]+\);\s*', '', line)

        cleaned_lines.append(line)

    # Remove any trailing empty lines that were left
    while cleaned_lines and cleaned_lines[-1].strip() == '':
        cleaned_lines.pop()

    return '\n'.join(cleaned_lines)


def main():
    parser = argparse.ArgumentParser(description='Simple C++ coverage instrumenter using regex')
    parser.add_argument('input_file', help='Input C++ file')
    parser.add_argument('-o', '--output', help='Output file (default: overwrites input file)')
    parser.add_argument('--header-only', action='store_true', help='Only output the coverage header')
    parser.add_argument('--remove', action='store_true', help='Remove coverage instrumentation only')
    parser.add_argument('--debug', action='store_true', help='Enable debug output')

    args = parser.parse_args()

    # Read input file
    try:
        with open(args.input_file, 'r') as f:
            source_code = f.read()
    except FileNotFoundError:
        print(f"Error: File '{args.input_file}' not found")
        sys.exit(1)

    if args.remove:
        # Remove coverage mode only
        output = remove_coverage(source_code)

        # Determine output file
        if args.output:
            output_file = args.output
        else:
            output_file = args.input_file  # Overwrite input file

        with open(output_file, 'w') as f:
            f.write(output)

        print(f"Coverage removed. Output written to: {output_file}")
    else:
        # First remove any existing coverage
        clean_code = remove_coverage(source_code)

        # Then instrument the clean code
        instrumenter = SimpleCoverageInstrumenter(clean_code, debug=args.debug)
        instrumented_code = instrumenter.instrument()

        # Generate output
        if args.header_only:
            output = instrumenter.generate_coverage_header()
        else:
            output = instrumenter.generate_coverage_header() + instrumented_code

        # Determine output file
        if args.output:
            output_file = args.output
        else:
            output_file = args.input_file  # Overwrite input file

        with open(output_file, 'w') as f:
            f.write(output)

        print(f"Instrumented code written to: {output_file}")
        print(f"Total coverage points added: {len(instrumenter.coverage_points)}")

        # Display coverage points
        print("\nCoverage points added:")
        for point in sorted(instrumenter.coverage_points):
            print(f"  {point}")

if __name__ == '__main__':
    main()
